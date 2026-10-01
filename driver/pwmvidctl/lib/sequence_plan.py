#!/usr/bin/env python3
"""Compile a CB PWMVID sequence onto absolute PWM-period boundaries.

This compiler is offline. The CB output image accepts the generated verbs;
this program itself never opens a serial port or drives hardware.
"""
from __future__ import annotations

import argparse
import binascii
import json
import struct
from decimal import Decimal, InvalidOperation
from pathlib import Path

try:
    from .pwmvid import validate_profile
except ImportError:
    from pwmvid import validate_profile

PWM_HZ = 675_000
TIMER_PERIOD_TICKS = 240
NS_PER_SECOND = 1_000_000_000
DEFAULT_MIN_CCR = 2  # User-approved experimental 1% planning range.
INSTALLED_MIN_CCR = 69
DEFAULT_MAX_CCR = 224
MAX_SEGMENTS = 32
MAX_PERIODS = 0xFFFF_FFFF
COMMAND_FORMAT = 1
SUSTAINED_PERIODS = 3375   # 5 ms
SHORT_BURST_MAX = 6750     # 10 ms
EXPERIMENTAL_LOW_MAX = 8_100_000  # 12 s at 675 kHz


def _integer(value: object, label: str) -> int:
    if type(value) is not int:
        raise ValueError(f"{label} must be an integer")
    return value


def duty_to_ccr(value: object) -> int:
    """Round decimal percentage to the nearest timer tick, ties upward."""
    if type(value) not in (str, int):
        raise ValueError("duty_pct must be a decimal string or integer")
    try:
        duty = Decimal(str(value))
    except (InvalidOperation, ValueError) as exc:
        raise ValueError("duty_pct must be a decimal percentage") from exc
    if not duty.is_finite() or duty < 0 or duty > 100:
        raise ValueError("duty_pct must be within 0..100")
    return int((duty * TIMER_PERIOD_TICKS / 100 + Decimal("0.5")).to_integral_value(rounding="ROUND_FLOOR"))


def voltage_to_ccr(value: object, profile: dict) -> int:
    """Use the firmware SETMV endpoint interpolation, including signed ties."""
    mv = _integer(value, "mv")
    if not profile["mv1"] <= mv <= profile["mv2"]:
        raise ValueError("mv lies outside the measured profile endpoints")
    numerator = (mv - profile["mv1"]) * (profile["ccr2"] - profile["ccr1"])
    denominator = profile["mv2"] - profile["mv1"]
    rounded = (abs(numerator) + denominator // 2) // denominator
    return profile["ccr1"] + (rounded if numerator >= 0 else -rounded)


def _target(value: object, min_ccr: int, max_ccr: int,
            profile: dict | None) -> int:
    if value == "lowest":
        ccr = min_ccr
    elif isinstance(value, dict) and set(value) == {"ccr"}:
        ccr = _integer(value["ccr"], "ccr")
    elif isinstance(value, dict) and set(value) == {"duty_pct"}:
        ccr = duty_to_ccr(value["duty_pct"])
    elif isinstance(value, dict) and set(value) == {"mv"}:
        if profile is None:
            raise ValueError("mv targets require a measured --profile")
        ccr = voltage_to_ccr(value["mv"], profile)
    else:
        raise ValueError("target must be 'lowest', {'ccr': N}, {'duty_pct': 'N'}, or {'mv': N}")
    if not min_ccr <= ccr <= max_ccr:
        raise ValueError(f"CCR {ccr} is outside the selected {min_ccr}..{max_ccr} envelope")
    return ccr


def _boundary(cumulative_ns: int) -> int:
    # All endpoints round from the same time origin, so individual durations
    # never accumulate their own rounding errors.
    return (cumulative_ns * PWM_HZ + NS_PER_SECOND // 2) // NS_PER_SECOND


def compile_plan(spec: dict, *, min_ccr: int = DEFAULT_MIN_CCR,
                 max_ccr: int = DEFAULT_MAX_CCR,
                 profile: dict | None = None) -> dict:
    if type(spec) is not dict or set(spec) != {"background", "steps", "post"}:
        raise ValueError("plan requires exactly background, steps, and post")
    if type(min_ccr) is not int or type(max_ccr) is not int or not 1 <= min_ccr <= max_ccr < 240:
        raise ValueError("invalid CCR envelope")
    if profile is not None:
        validate_profile(profile)
    steps = spec["steps"]
    if type(steps) is not list or not 1 <= len(steps) <= MAX_SEGMENTS:
        raise ValueError(f"steps must contain 1..{MAX_SEGMENTS} segments")
    background = _target(spec["background"], min_ccr, max_ccr, profile)
    post = _target(spec["post"], min_ccr, max_ccr, profile)
    if background < INSTALLED_MIN_CCR or post < INSTALLED_MIN_CCR:
        raise ValueError("background and post must be CCR69 or above for capture feedback")
    cumulative_ns = 0
    previous_period = 0
    short_span = 0
    compiled = []
    for index, step in enumerate(steps):
        if type(step) is not dict or set(step) != {"target", "duration_ns"}:
            raise ValueError(f"step {index} requires exactly target and duration_ns")
        ccr = _target(step["target"], min_ccr, max_ccr, profile)
        duration_ns = _integer(step["duration_ns"], "duration_ns")
        if duration_ns <= 0:
            raise ValueError(f"step {index} duration_ns must be positive")
        cumulative_ns += duration_ns
        end_period = _boundary(cumulative_ns)
        if end_period <= previous_period:
            raise ValueError(f"step {index} is shorter than one representable PWM period")
        if end_period > MAX_PERIODS:
            raise ValueError("sequence exceeds 32-bit PWM-period index")
        periods = end_period - previous_period
        if ccr < INSTALLED_MIN_CCR and periods > EXPERIMENTAL_LOW_MAX:
            raise ValueError(f"step {index} experimental hold exceeds 12 s")
        if periods < SUSTAINED_PERIODS:
            short_span += periods
            if short_span > SHORT_BURST_MAX:
                raise ValueError("consecutive short steps exceed 10 ms without feedback")
        else:
            short_span = 0
        compiled.append({"index": index, "ccr": ccr,
                         "start_period": previous_period, "end_period": end_period,
                         "periods": periods,
                         "feedback": ("experimental_rail_only" if ccr < INSTALLED_MIN_CCR else
                                      "sustained_capture" if periods >= SUSTAINED_PERIODS else
                                      "unverified_short"),
                         "requested_end_ns": cumulative_ns,
                         "end_error_ns_numerator": end_period * NS_PER_SECOND - cumulative_ns * PWM_HZ})
        previous_period = end_period
    experimental = any(ccr < INSTALLED_MIN_CCR for ccr in
                       [background, post, *(step["ccr"] for step in compiled)])
    return {"format": "pwmvid-sequence-plan-v1", "pwm_hz": PWM_HZ,
            "timer_period_ticks": TIMER_PERIOD_TICKS,
            "envelope": {"min_ccr": min_ccr, "max_ccr": max_ccr},
            "experimental_low_duty": experimental,
            "installed_firmware_min_ccr": INSTALLED_MIN_CCR,
            "background_ccr": background, "post_ccr": post,
            "total_periods": previous_period, "steps": compiled,
            "end_error_ns_denominator": PWM_HZ,
            "hardware_status": "OFFLINE_PLAN_DEVICE_EXECUTION_UNVERIFIED"}


def command_payload(plan: dict, sequence_id: int) -> bytes:
    """Canonical little-endian upload bytes, independent of JSON formatting.

    The CB output image implements the matching SEQ parser.
    """
    if type(sequence_id) is not int or not 1 <= sequence_id <= 0xFFFF_FFFF:
        raise ValueError("sequence_id must be a nonzero 32-bit integer")
    if plan.get("format") != "pwmvid-sequence-plan-v1":
        raise ValueError("unsupported compiled plan")
    steps = plan.get("steps")
    if type(steps) is not list or not 1 <= len(steps) <= MAX_SEGMENTS:
        raise ValueError("invalid compiled step count")
    if plan.get("total_periods") != steps[-1].get("end_period"):
        raise ValueError("compiled total_periods mismatch")
    payload = bytearray(struct.pack("<BIHHBI", COMMAND_FORMAT, sequence_id,
                                    plan["background_ccr"], plan["post_ccr"],
                                    len(steps), plan["total_periods"]))
    previous = 0
    for index, step in enumerate(steps):
        end = step.get("end_period")
        ccr = step.get("ccr")
        if (step.get("index") != index or type(end) is not int or
                not previous < end <= MAX_PERIODS or type(ccr) is not int or
                not 1 <= ccr < TIMER_PERIOD_TICKS):
            raise ValueError(f"invalid compiled step {index}")
        payload.extend(struct.pack("<IH", end, ccr))
        previous = end
    return bytes(payload)


def upload_commands(plan: dict, sequence_id: int) -> dict:
    """Generate session command verbs; deliberately never send them."""
    payload = command_payload(plan, sequence_id)
    digest = binascii.crc32(payload) & 0xFFFF_FFFF
    verbs = [f"SETBG {plan['background_ccr']}",
             f"SEQ BEGIN {sequence_id} {len(plan['steps'])} {plan['post_ccr']} {plan['total_periods']}"]
    verbs.extend(f"SEQ ADD {sequence_id} {step['index']} {step['end_period']} {step['ccr']}"
                 for step in plan["steps"])
    verbs.append(f"SEQ COMMIT {sequence_id} {digest}")
    if any(len(verb) + 22 > 255 for verb in verbs):
        raise ValueError("command could exceed the session line limit")
    return {"hardware_status": "OFFLINE_PLAN_DEVICE_EXECUTION_UNVERIFIED", "sequence_id": sequence_id,
            "digest_crc32": digest, "upload_verbs": verbs,
            "start_verb": f"SEQ START {sequence_id}",
            "status_verb": "SEQ STATUS", "cancel_verb": f"SEQ CANCEL {sequence_id}"}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("spec", type=Path, help="JSON input plan")
    parser.add_argument("--min-ccr", type=int, default=DEFAULT_MIN_CCR,
                        help="planning envelope only; does not modify firmware")
    parser.add_argument("--max-ccr", type=int, default=DEFAULT_MAX_CCR)
    parser.add_argument("--commands", action="store_true",
                        help="print CB upload verbs; never connects to hardware")
    parser.add_argument("--sequence-id", type=int, default=1)
    parser.add_argument("--profile", type=Path,
                        help="measured profile JSON, required for {'mv': N} targets")
    args = parser.parse_args()
    try:
        profile = json.loads(args.profile.read_text()) if args.profile else None
        plan = compile_plan(json.loads(args.spec.read_text()), min_ccr=args.min_ccr,
                            max_ccr=args.max_ccr, profile=profile)
        result = upload_commands(plan, args.sequence_id) if args.commands else plan
    except (OSError, json.JSONDecodeError, ValueError) as exc:
        parser.error(str(exc))
    print(json.dumps(result, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
