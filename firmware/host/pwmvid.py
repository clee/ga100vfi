#!/usr/bin/env python3
"""Deliberate fixture-only USB CDC client. No ARM retry or reconnect path."""
from __future__ import annotations
import argparse
import json
import secrets
import sys
import time
from pathlib import Path

PROFILE_KEYS = (
    "mv1", "ccr1", "mv2", "ccr2", "gain_ppm", "offset_mv",
    "boot_min_mv", "boot_max_mv", "load_min_mv", "load_max_mv",
)


class ProtocolError(RuntimeError):
    pass


def validate_profile(profile: dict) -> dict:
    if not isinstance(profile, dict) or set(profile) != set(PROFILE_KEYS):
        raise ValueError("Profile must contain exactly: " + ", ".join(PROFILE_KEYS))
    if any(type(profile[k]) is not int for k in PROFILE_KEYS):
        raise ValueError("All profile fields must be integers")
    p = profile
    if not (100 <= p["mv1"] < p["mv2"] <= 1200):
        raise ValueError("Measured endpoint mV must increase within 100..1200")
    if not (16 <= p["ccr1"] <= 224 and 16 <= p["ccr2"] <= 224 and p["ccr1"] != p["ccr2"]):
        raise ValueError("Distinct measured endpoint CCR must be within 16..224")
    if not (100000 <= p["gain_ppm"] <= 10000000 and -1200 <= p["offset_mv"] <= 1200):
        raise ValueError("ADC compensation is out of accepted range")
    for mode in ("boot", "load"):
        if not (100 <= p[f"{mode}_min_mv"] <= p[f"{mode}_max_mv"] <= 1200):
            raise ValueError(f"Invalid {mode} rail plausibility bounds")
    return p


class Client:
    def __init__(self, port):
        self.port = port
        self.session = None
        self.sequence = 0

    def exchange(self, line: str) -> str:
        data = line.encode("ascii")
        if not data or len(data) > 255 or b"\r" in data or b"\n" in data:
            raise ValueError("Invalid command line")
        self.port.write(data + b"\n")
        self.port.flush()
        response = self.port.readline()
        if not response or not response.endswith(b"\n"):
            raise ProtocolError("Missing/partial response; command is NOT retried")
        try:
            text = response.decode("ascii").rstrip("\r\n")
        except UnicodeDecodeError as exc:
            raise ProtocolError("Non-ASCII response") from exc
        if not text.startswith("OK "):
            raise ProtocolError(text)
        return text

    def hello(self):
        nonce = secrets.randbits(32)
        response = self.exchange(f"HELLO {nonce}")
        fields = response.split()
        if len(fields) != 4 or fields[:2] != ["OK", "HELLO"]:
            raise ProtocolError("Invalid handshake")
        try:
            session, returned_nonce = map(int, fields[2:])
        except ValueError as exc:
            raise ProtocolError("Invalid handshake numbers") from exc
        if not 1 <= session <= 0xFFFFFFFF or returned_nonce != nonce:
            raise ProtocolError("Handshake mismatch")
        self.session, self.sequence = session, 0

    def command(self, verb: str) -> str:
        if self.session is None:
            raise ProtocolError("HELLO required")
        if self.sequence == 0xFFFFFFFF:
            raise ProtocolError("Sequence exhausted; stop and begin a new deliberate session")
        self.sequence += 1
        return self.exchange(f"{self.session} {self.sequence} {verb}")

    def stop(self) -> str:
        return self.exchange("STOP")


def wait_source_ready(client, timeout_s=0.5):
    deadline = time.monotonic() + timeout_s
    while time.monotonic() < deadline:
        response = client.exchange("TELEM")
        fields = dict(token.split("=", 1) for token in response.split()[2:] if "=" in token)
        if fields.get("state") != "PREARM":
            raise ProtocolError("PREARM ended before source ready: " + response)
        if fields.get("source_ready") == "1":
            return response
        time.sleep(0.01)
    raise ProtocolError("Source not ready within bounded preview wait; ARM was not sent")


def enter_dfu(client, *, confirmed=False):
    # Neither confirmation nor GPU disconnection resolves the on-board PA6 conflict.
    raise ProtocolError("DFU unavailable: factory ROM PA6 conflicts with the board receiver; no command sent")


def preview(client, duration_s):
    if not 0 < duration_s <= 3600:
        raise ValueError("Preview duration must be >0 and <=3600 seconds")
    try:
        print(client.command("PREARM"), flush=True)
        print(wait_source_ready(client), flush=True)
        deadline = time.monotonic() + duration_s
        while time.monotonic() < deadline:
            print(client.exchange("TELEM"), flush=True)
            time.sleep(min(0.25, max(0, deadline-time.monotonic())))
    finally:
        print(client.stop(), flush=True)


def run_fixture(client: Client, profile: dict, *, mv: int | None, ccr: int | None,
                lease_ms: int, duration_s: float, fixture: bool, reset_fault: bool = False):
    if not fixture:
        raise ValueError("--fixture is mandatory and acknowledges an isolated test fixture")
    validate_profile(profile)
    if not 100 <= lease_ms <= 500:
        raise ValueError("CLI lease must be 100..500ms")
    if not 0 < duration_s <= 3600:
        raise ValueError("Duration must be greater than zero and at most 3600 seconds")
    if (mv is None) == (ccr is None):
        raise ValueError("Exactly one target is required")
    if mv is not None and not profile["mv1"] <= mv <= profile["mv2"]:
        raise ValueError("Voltage target lies outside measured endpoints")
    lo, hi = sorted((profile["ccr1"], profile["ccr2"]))
    if ccr is not None and not lo <= ccr <= hi:
        raise ValueError("Raw target lies outside measured CCR endpoints")
    try:
        status = client.command("STATUS")
        if status.startswith("OK STATUS FAULT "):
            if not reset_fault:
                raise ProtocolError("FAULT is latched; inspect the fixture, then explicitly use --reset-fault")
            client.command("RESET")
        elif not status.startswith("OK STATUS PASSIVE "):
            raise ProtocolError("Expected PASSIVE before fixture qualification: " + status)
        client.command("QUALIFY FIXTURE_ISOLATED")
        client.command("PROFILE " + " ".join(str(profile[k]) for k in PROFILE_KEYS))
        client.command("PREARM")
        wait_source_ready(client)
        arm_deadline = time.monotonic() + lease_ms / 1000.0 * 0.7
        print(client.command(f"ARM {lease_ms}"), flush=True)
        # No retries, including a lost ARM response. STATUS is observation only.
        status = client.command("STATUS")
        while status.startswith("OK STATUS ARMING ") and time.monotonic() < arm_deadline:
            time.sleep(0.005)
            status = client.command("STATUS")
        if not status.startswith("OK STATUS ARMED "):
            raise ProtocolError("Hardware has not acknowledged arming: " + status)
        print(status, flush=True)
        # Change the host target only after physical arming acknowledged.
        client.command(f"SETMV {mv}" if mv is not None else f"SETCCR {ccr}")
        deadline = time.monotonic() + duration_s
        interval = min(0.1, lease_ms / 3000.0)
        while time.monotonic() < deadline:
            client.command("PING")
            time.sleep(min(interval, max(0, deadline - time.monotonic())))
    finally:
        # A best-effort physical-safe request, never a renewed ARM. If the USB
        # link is gone, firmware lease expiration and hardware interlocks apply.
        try:
            print(client.stop(), flush=True)
        except Exception as exc:
            raise ProtocolError(f"STOP unconfirmed: {exc}; verify RUN is low, U4 is disabled and fallback is independently checked") from exc


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", required=True, help="Explicit USB CDC device path")
    sub = parser.add_subparsers(dest="action", required=True)
    for name in ("status", "stop", "reset", "telem", "raw"):
        sub.add_parser(name)
    prearm = sub.add_parser("prearm", help="Hold a bounded internal-source preview with output gated")
    prearm.add_argument("--duration", type=float, default=5.0)
    loader = sub.add_parser("usb-loader", help="Enter the board-safe CUSTOM USB updater; does not upload firmware")
    loader.add_argument("--confirm-update", action="store_true", help="Authorize custom-loader reboot with GPU disconnected, PASSIVE/RUN low, and stable power; JP1 may stay bridged")
    dfu = sub.add_parser("dfu", help="BLOCKED: factory ROM DFU is electrically incompatible with this board")
    dfu.add_argument("--confirm-dfu", action="store_true", help="Legacy flag; factory ROM DFU stays blocked regardless of confirmation")
    run = sub.add_parser("run", help="Deliberately arm and maintain a bounded fixture lease")
    run.add_argument("--fixture", action="store_true", help="Acknowledge isolated fixture, no GPU connected")
    run.add_argument("--reset-fault", action="store_true",
                     help="Explicitly clear a latched fault after inspecting the isolated fixture")
    run.add_argument("--profile", type=Path, required=True, help="Your measured calibration JSON")
    target = run.add_mutually_exclusive_group(required=True)
    target.add_argument("--mv", type=int)
    target.add_argument("--ccr", type=int)
    run.add_argument("--lease-ms", type=int, default=300)
    run.add_argument("--duration", type=float, required=True)
    args = parser.parse_args(argv)
    if args.action == "run" and not args.fixture:
        parser.error("run requires --fixture; use only an electrically isolated qualification fixture")
    if args.action == "usb-loader" and not args.confirm_update:
        parser.error("usb-loader requires --confirm-update; disconnect GPU and open JP1 first")
    if args.action == "dfu":
        parser.error("USB ROM DFU unavailable on this board revision: ROM PA6 SPI output conflicts with the on-board receiver. No reboot or upload attempted. Use usb-loader for the separately installed board-safe custom updater, or the reviewed ST-Link SWD path.")
    try:
        import serial
    except ImportError:
        parser.error("Install the host dependency: python -m pip install -r host/requirements.txt")
    try:
        # Baudrate is CDC line coding only. Never scan/connect to arbitrary ports.
        with serial.Serial(args.port, 115200, timeout=0.15, write_timeout=0.15) as port:
            port.reset_input_buffer()
            client = Client(port)
            if args.action == "stop":
                print(client.stop())
                return 0
            if args.action in ("telem", "raw"):
                print(client.exchange(args.action.upper()))
                return 0
            if args.action != "usb-loader":
                client.hello()  # Always disarms; even status creates a new safe session.
            if args.action == "run":
                profile = validate_profile(json.loads(args.profile.read_text()))
                run_fixture(client, profile, mv=args.mv, ccr=args.ccr,
                            lease_ms=args.lease_ms, duration_s=args.duration, fixture=args.fixture,
                            reset_fault=args.reset_fault)
            elif args.action == "prearm":
                preview(client, args.duration)
            elif args.action == "usb-loader":
                from usb_update import enter_custom_loader
                enter_custom_loader(client)
                print("Custom USB updater requested; disconnect expected. No firmware uploaded. Use dfu-util -l to obtain its exact serial and verify the PWMVID-P8P9-APP descriptor.")
            else:
                print(client.command(args.action.upper()))
        return 0
    except KeyboardInterrupt:
        print("Interrupted; verify RUN is low and the output is disabled", file=sys.stderr)
        return 130
    except (OSError, ValueError, ProtocolError, serial.SerialException) as exc:
        print(str(exc), file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
