#!/usr/bin/env python3
"""Upload and run one bounded CB timed sequence over the PWMVID CDC session."""
from __future__ import annotations

import argparse
import json
import secrets
import sys
import time
from pathlib import Path

try:
    from .pwmvid import Client, PROFILE_KEYS, ProtocolError, validate_profile, wait_source_ready
    from .sequence_plan import compile_plan, upload_commands
except ImportError:
    from pwmvid import Client, PROFILE_KEYS, ProtocolError, validate_profile, wait_source_ready
    from sequence_plan import compile_plan, upload_commands


def fields(response: str) -> dict[str, str]:
    return dict(item.split("=", 1) for item in response.split()[2:] if "=" in item)


def check_status(client: Client, expected: str) -> str:
    response = client.command("STATUS")
    if not response.startswith(f"OK STATUS {expected} "):
        raise ProtocolError(f"Expected {expected}: {response}")
    return response


def keepalive(client: Client, last_ping: float, lease_ms: int) -> float:
    if time.monotonic() - last_ping >= min(0.08, lease_ms / 4000.0):
        client.command("PING")
        return time.monotonic()
    return last_ping


def wait_feedback(client: Client, lease_ms: int, timeout_s: float,
                  last_ping: float, *, completed_id: int | None = None) -> float:
    deadline = time.monotonic() + timeout_s
    while time.monotonic() < deadline:
        last_ping = keepalive(client, last_ping, lease_ms)
        check_status(client, "ARMED")
        info = fields(client.command("SEQ STATUS"))
        if info.get("run") != "1" or info.get("active") != "1":
            raise ProtocolError("Sequence hardware is no longer active")
        if info.get("running") == "0" and info.get("feedback") == "1":
            if completed_id is None or info.get("id") == str(completed_id):
                return last_ping
        time.sleep(0.02)
    raise ProtocolError("Timed sequence or feedback did not finish before host deadline")


def prepare_sequence(profile: dict, spec: dict, sequence_id: int) -> tuple[dict, dict]:
    validate_profile(profile)
    plan = compile_plan(spec, profile=profile)
    if not 1 <= sequence_id <= 0xFFFF_FFFF:
        raise ValueError("Sequence ID must be a nonzero 32-bit number")
    commands = upload_commands(plan, sequence_id)
    # The measured profile covers steady background and post duty. Experimental
    # CCR2..15 segments are explicitly outside the measured feedback envelope.
    lo, hi = sorted((profile["ccr1"], profile["ccr2"]))
    if not lo <= plan["background_ccr"] <= hi or not lo <= plan["post_ccr"] <= hi:
        raise ValueError("Background and post duty must lie within measured profile endpoints")
    if any(step["ccr"] >= 16 and not lo <= step["ccr"] <= hi for step in plan["steps"]):
        raise ValueError("Normal-range sequence duties must lie within measured profile endpoints")
    return plan, commands


def arm_session(client: Client, profile: dict, qualification: str,
                lease_ms: int) -> float:
    validate_profile(profile)
    if qualification not in ("FIXTURE_ISOLATED", "GPU_EXPERIMENT"):
        raise ValueError("Explicit fixture or GPU experiment qualification required")
    if not 100 <= lease_ms <= 500:
        raise ValueError("Lease must be 100..500ms")
    check_status(client, "PASSIVE")
    if not client.command("SEQ STATUS").startswith("OK SEQ_STATUS "):
        raise ProtocolError("CB sequence firmware is not present")
    print(client.command(f"QUALIFY {qualification}"), flush=True)
    print(client.command("PROFILE " + " ".join(str(profile[k]) for k in PROFILE_KEYS)), flush=True)
    print(client.command("PREARM"), flush=True)
    print(wait_source_ready(client), flush=True)
    print(client.command(f"ARM {lease_ms}"), flush=True)
    deadline = time.monotonic() + lease_ms * 0.7 / 1000.0
    while time.monotonic() < deadline:
        status = client.command("STATUS")
        if status.startswith("OK STATUS ARMED "):
            break
        if not status.startswith("OK STATUS ARMING "):
            raise ProtocolError("ARM did not complete: " + status)
        time.sleep(0.005)
    else:
        raise ProtocolError("ARM did not complete before bounded deadline")
    print(status, flush=True)
    return wait_feedback(client, lease_ms, 0.5, time.monotonic())


def execute_sequence(client: Client, plan: dict, commands: dict, *,
                     lease_ms: int, last_ping: float) -> float:
    print(client.command(f"SETBG {plan['background_ccr']}"), flush=True)
    last_ping = wait_feedback(client, lease_ms, 0.5, last_ping)
    for verb in commands["upload_verbs"][1:]:
        last_ping = keepalive(client, last_ping, lease_ms)
        print(client.command(verb), flush=True)
    last_ping = keepalive(client, last_ping, lease_ms)
    # START is deliberately one-shot: a lost reply must be resolved with
    # SEQ STATUS, never by automatically sending START again.
    print(client.command(commands["start_verb"]), flush=True)
    total_s = plan["total_periods"] / plan["pwm_hz"]
    last_ping = wait_feedback(client, lease_ms, total_s + 1.0, last_ping,
                              completed_id=commands["sequence_id"])
    print("Sequence finished and final background feedback verified", flush=True)
    return last_ping


def run_sequence(client: Client, profile: dict, spec: dict, *, sequence_id: int,
                 lease_ms: int, post_hold_s: float, qualification: str) -> None:
    if not 0 <= post_hold_s <= 3600:
        raise ValueError("Post hold must be 0..3600s")
    plan, commands = prepare_sequence(profile, spec, sequence_id)
    try:
        last_ping = arm_session(client, profile, qualification, lease_ms)
        last_ping = execute_sequence(client, plan, commands,
                                     lease_ms=lease_ms, last_ping=last_ping)
        deadline = time.monotonic() + post_hold_s
        while time.monotonic() < deadline:
            last_ping = keepalive(client, last_ping, lease_ms)
            time.sleep(0.02)
    finally:
        try:
            print(client.stop(), flush=True)
        except Exception as exc:
            raise ProtocolError(f"STOP unconfirmed during sequence session: {exc}; check RUN and output gate") from exc


def main(argv=None) -> int:
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--port", required=True, help="Explicit PWMVID G431CB CDC device path")
    p.add_argument("--profile", type=Path, required=True, help="Measured calibration JSON")
    p.add_argument("--plan", type=Path, required=True, help="Timed-sequence JSON")
    mode = p.add_mutually_exclusive_group(required=True)
    mode.add_argument("--fixture", action="store_true", help="Physically isolated output test")
    mode.add_argument("--gpu-experiment", action="store_true", help="Explicit experimental GPU output")
    p.add_argument("--sequence-id", type=int, help="Nonzero ID; random if omitted")
    p.add_argument("--lease-ms", type=int, default=300)
    p.add_argument("--post-hold-s", type=float, default=1.0)
    args = p.parse_args(argv)
    try:
        import serial
    except ImportError:
        p.error("Install the host dependency: python -m pip install -r host/requirements.txt")
    try:
        profile = json.loads(args.profile.read_text())
        spec = json.loads(args.plan.read_text())
        sequence_id = args.sequence_id or secrets.randbelow(0xFFFF_FFFF) + 1
        qualification = "FIXTURE_ISOLATED" if args.fixture else "GPU_EXPERIMENT"
        with serial.Serial(args.port, 115200, timeout=0.15, write_timeout=0.15) as port:
            port.reset_input_buffer()
            client = Client(port)
            client.hello()  # Deliberately returns the board to PASSIVE.
            run_sequence(client, profile, spec, sequence_id=sequence_id,
                         lease_ms=args.lease_ms, post_hold_s=args.post_hold_s,
                         qualification=qualification)
        return 0
    except KeyboardInterrupt:
        print("Interrupted; verify RUN and the output gate are low", file=sys.stderr)
        return 130
    except (OSError, ValueError, ProtocolError, serial.SerialException) as exc:
        print(str(exc), file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
