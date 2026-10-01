#!/usr/bin/env python3
"""Keep one qualified CB session armed across successive timed sequences.

Run an initial JSON plan, then enter `plan /path/to/next.json`, `status`, or
`stop` on stdin. The host renews the firmware lease while waiting. Closing the
terminal or losing the serial link does not leave an indefinitely armed device.
"""
from __future__ import annotations

import argparse
import json
from pathlib import Path
import secrets
import select
import stat
import sys
from typing import Iterable

try:
    from .pwmvid import Client, ProtocolError
    from .sequence_run import (arm_session, check_status, execute_sequence,
                               keepalive, prepare_sequence)
except ImportError:
    from pwmvid import Client, ProtocolError
    from sequence_run import (arm_session, check_status, execute_sequence,
                              keepalive, prepare_sequence)


def load_plan(path: Path) -> dict:
    # One plan needs at most 32 short steps. Reject large or non-JSON files
    # before spending appreciable time away from the host lease loop.
    info = path.stat()
    if not stat.S_ISREG(info.st_mode):
        raise ValueError("Plan path must be a regular file")
    if info.st_size > 16_384:
        raise ValueError("Plan file exceeds 16 KiB")
    return json.loads(path.read_text())


def next_operations(profile: dict, lease_ms: int) -> Iterable[tuple[str, dict | None, int]]:
    print("Post-duty is held. Enter `plan /path/to/next.json`, `status`, or `stop`.", flush=True)
    while True:
        readable, _, _ = select.select([sys.stdin], [], [], min(0.05, lease_ms / 4000.0))
        if not readable:
            yield "idle", None, 0
            continue
        line = sys.stdin.readline()
        if not line:
            yield "stop", None, 0
            return
        command = line.strip()
        if command == "stop":
            yield "stop", None, 0
            return
        if command == "status":
            yield "status", None, 0
            continue
        if command.startswith("plan ") and command[5:].strip():
            try:
                spec = load_plan(Path(command[5:].strip()).expanduser())
                sequence_id = secrets.randbelow(0xFFFF_FFFF) + 1
                prepare_sequence(profile, spec, sequence_id)
            except (OSError, ValueError, json.JSONDecodeError) as exc:
                print(f"Invalid plan: {exc}", file=sys.stderr, flush=True)
                yield "idle", None, 0
                continue
            yield "plan", spec, sequence_id
            continue
        print("Use `plan /path/to/file.json`, `status`, or `stop`.", file=sys.stderr, flush=True)
        yield "idle", None, 0


def run_session(client: Client, profile: dict, first_spec: dict, *,
                first_id: int, qualification: str, lease_ms: int,
                operations: Iterable[tuple[str, dict | None, int]]) -> None:
    first_plan, first_commands = prepare_sequence(profile, first_spec, first_id)
    try:
        last_ping = arm_session(client, profile, qualification, lease_ms)
        last_ping = execute_sequence(client, first_plan, first_commands,
                                     lease_ms=lease_ms, last_ping=last_ping)
        for operation, spec, sequence_id in operations:
            last_ping = keepalive(client, last_ping, lease_ms)
            if operation == "idle":
                continue
            if operation == "stop":
                break
            if operation == "status":
                print(check_status(client, "ARMED"), flush=True)
                print(client.command("SEQ STATUS"), flush=True)
                continue
            if operation != "plan" or spec is None:
                raise ValueError("Unknown session operation")
            plan, commands = prepare_sequence(profile, spec, sequence_id)
            last_ping = execute_sequence(client, plan, commands,
                                         lease_ms=lease_ms, last_ping=last_ping)
            print("Post-duty is held; enter the next command or stop.", flush=True)
    finally:
        try:
            print(client.stop(), flush=True)
        except Exception as exc:
            raise ProtocolError(f"STOP unconfirmed: {exc}; check RUN and output gate") from exc


def main(argv=None) -> int:
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--port", required=True, help="Explicit PWMVID G431CB CDC port")
    p.add_argument("--profile", type=Path, required=True, help="Measured calibration JSON")
    p.add_argument("--first-plan", type=Path, required=True, help="First timed-sequence JSON")
    mode = p.add_mutually_exclusive_group(required=True)
    mode.add_argument("--fixture", action="store_true", help="Physically isolated output test")
    mode.add_argument("--gpu-experiment", action="store_true", help="Explicit experimental GPU output")
    p.add_argument("--lease-ms", type=int, default=300)
    args = p.parse_args(argv)
    try:
        import serial
    except ImportError:
        p.error("Install the host dependency: python -m pip install -r host/requirements.txt")
    try:
        profile = json.loads(args.profile.read_text())
        first_spec = load_plan(args.first_plan)
        first_id = secrets.randbelow(0xFFFF_FFFF) + 1
        prepare_sequence(profile, first_spec, first_id)
        qualification = "FIXTURE_ISOLATED" if args.fixture else "GPU_EXPERIMENT"
        with serial.Serial(args.port, 115200, timeout=0.15, write_timeout=0.15) as port:
            port.reset_input_buffer()
            client = Client(port)
            client.hello()
            run_session(client, profile, first_spec, first_id=first_id,
                        qualification=qualification, lease_ms=args.lease_ms,
                        operations=next_operations(profile, args.lease_ms))
        return 0
    except KeyboardInterrupt:
        print("Interrupted; STOP was requested, verify RUN is low", file=sys.stderr)
        return 130
    except (OSError, ValueError, ProtocolError, serial.SerialException) as exc:
        print(str(exc), file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
