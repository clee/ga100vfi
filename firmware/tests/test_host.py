"""Offline host tests. Synthetic fixtures are not board calibration."""
import contextlib
import importlib.util
import io
from pathlib import Path
import unittest
from unittest.mock import patch

SPEC = importlib.util.spec_from_file_location("pwmvid", Path(__file__).parents[1] / "host/pwmvid.py")
pwmvid = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(pwmvid)
PROFILE = dict(zip(pwmvid.PROFILE_KEYS, (200, 16, 1100, 224, 1000000, 0, 700, 900, 100, 1200)))


class FakePort:
    def __init__(self, replies=()):
        self.replies = list(replies)
        self.writes = []
    def write(self, data): self.writes.append(data)
    def flush(self): pass
    def readline(self): return self.replies.pop(0) if self.replies else b""


class FakeClient:
    def __init__(self, fail_arm=False, arm_state="ARMED", fail_stop=False, initial_state="PASSIVE"):
        self.commands = []
        self.fail_arm, self.arm_state, self.fail_stop = fail_arm, arm_state, fail_stop
        self.initial_state = initial_state
    def command(self, text):
        self.commands.append(text)
        if text.startswith("ARM ") and self.fail_arm:
            raise pwmvid.ProtocolError("ARM response lost")
        if text == "STATUS":
            state = self.arm_state if any(c.startswith("ARM ") for c in self.commands) else self.initial_state
            return f"OK STATUS {state} fault=0"
        return "OK " + text.split()[0]
    def exchange(self, text):
        self.commands.append(text)
        return "OK TELEM state=PREARM source_ready=1"
    def stop(self):
        self.commands.append("STOP")
        if self.fail_stop: raise pwmvid.ProtocolError("USB lost")
        return "OK STOP PASSIVE"


class HostTests(unittest.TestCase):
    def test_handshake_and_sequence(self):
        port = FakePort((b"OK HELLO 2 99\r\n", b"OK STATUS PASSIVE\n"))
        client = pwmvid.Client(port)
        with patch.object(pwmvid.secrets, "randbits", return_value=99): client.hello()
        self.assertEqual(client.command("STATUS"), "OK STATUS PASSIVE")
        self.assertEqual(port.writes, [b"HELLO 99\n", b"2 1 STATUS\n"])

    def test_bad_handshake(self):
        for response in (b"OK HELLO 2 98\n", b"OK HELLO 0 99\n", b"OK X\n", b"OK HELLO x 99\n"):
            client = pwmvid.Client(FakePort((response,)))
            with patch.object(pwmvid.secrets, "randbits", return_value=99):
                with self.assertRaises(pwmvid.ProtocolError): client.hello()
            self.assertIsNone(client.session)

    def test_no_retry_on_timeout_or_bad_reply(self):
        for reply in (b"", b"OK partial", b"ERR STATE\n", b"\xff\n"):
            port = FakePort((reply,)); client = pwmvid.Client(port); client.session=3
            with self.assertRaises(pwmvid.ProtocolError): client.command("ARM 300")
            self.assertEqual(port.writes, [b"3 1 ARM 300\n"])

    def test_command_bounds(self):
        port=FakePort(); client=pwmvid.Client(port)
        with self.assertRaises(pwmvid.ProtocolError): client.command("STATUS")
        client.session=1; client.sequence=0xFFFFFFFF
        with self.assertRaises(pwmvid.ProtocolError): client.command("STATUS")
        for command in ("", "x"*256, "STATUS\nARM 300", "STOP\r"):
            with self.assertRaises(ValueError): client.exchange(command)
        self.assertEqual(port.writes, [])

    def test_profile_bounds_and_types(self):
        self.assertEqual(pwmvid.validate_profile(PROFILE.copy()), PROFILE)
        for key,value in (("mv1",99),("mv2",1201),("ccr1",15),("ccr2",225),
                          ("gain_ppm",0),("offset_mv",1201),("boot_max_mv",699),
                          ("load_min_mv",1201),("mv1",True),("mv1",None)):
            profile=PROFILE.copy(); profile[key]=value
            with self.assertRaises(ValueError): pwmvid.validate_profile(profile)
        profile=PROFILE.copy(); profile["ccr2"]=profile["ccr1"]
        with self.assertRaises(ValueError): pwmvid.validate_profile(profile)
        profile=PROFILE.copy(); profile["extra"]=1
        with self.assertRaises(ValueError): pwmvid.validate_profile(profile)

    def run_args(self, client, **overrides):
        args=dict(mv=650,ccr=None,lease_ms=300,duration_s=0.001,fixture=True)
        args.update(overrides)
        with contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
            pwmvid.run_fixture(client, PROFILE.copy(), **args)

    def test_fixture_required_before_any_command(self):
        for overrides in (dict(fixture=False),dict(mv=1200),dict(ccr=1,mv=None),
                          dict(lease_ms=501),dict(duration_s=0),dict(ccr=120)):
            client=FakeClient()
            with self.assertRaises(ValueError): self.run_args(client, **overrides)
            self.assertEqual(client.commands, [])

    def test_arm_failure_only_stops(self):
        client=FakeClient(fail_arm=True)
        with self.assertRaises(pwmvid.ProtocolError): self.run_args(client)
        self.assertEqual(sum(c.startswith("ARM ") for c in client.commands), 1)
        self.assertEqual(client.commands[-1], "STOP")
        self.assertNotIn("PING",client.commands)

    def test_pending_arm_is_not_retried(self):
        client=FakeClient(arm_state="ARMING")
        with self.assertRaises(pwmvid.ProtocolError): self.run_args(client)
        self.assertEqual(sum(c.startswith("ARM ") for c in client.commands),1)
        self.assertEqual(client.commands[-1],"STOP")

    def test_bounded_run_stops(self):
        client=FakeClient()
        self.run_args(client)
        self.assertEqual(client.commands[:2], ["STATUS", "QUALIFY FIXTURE_ISOLATED"])
        self.assertEqual(sum(c.startswith("ARM ") for c in client.commands),1)
        self.assertLess(client.commands.index("PREARM"), client.commands.index("ARM 300"))
        self.assertGreater(client.commands.index("SETMV 650"), client.commands.index("ARM 300"))
        self.assertIn("PING",client.commands)
        self.assertEqual(client.commands[-1],"STOP")

    def test_fault_requires_explicit_reset(self):
        client=FakeClient(initial_state="FAULT")
        with self.assertRaises(pwmvid.ProtocolError): self.run_args(client)
        self.assertEqual(client.commands,["STATUS","STOP"])
        client=FakeClient(initial_state="FAULT")
        self.run_args(client, reset_fault=True)
        self.assertEqual(client.commands[:3],["STATUS","RESET","QUALIFY FIXTURE_ISOLATED"])
        self.assertEqual(client.commands.count("RESET"),1)

    def test_dfu_requires_confirmation_and_exact_reply(self):
        client = FakeClient()
        with self.assertRaises(pwmvid.ProtocolError): pwmvid.enter_dfu(client)
        self.assertEqual(client.commands, [])
        with self.assertRaises(pwmvid.ProtocolError): pwmvid.enter_dfu(client, confirmed=True)
        self.assertEqual(client.commands, [])

    def test_stop_failure_is_reported(self):
        client=FakeClient(fail_stop=True)
        with self.assertRaisesRegex(pwmvid.ProtocolError,"STOP unconfirmed"):
            self.run_args(client)
        self.assertEqual(client.commands.count("STOP"),1)


if __name__ == "__main__": unittest.main()
