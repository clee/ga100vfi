import unittest

from host.pwmvid import ProtocolError
from host.sequence_run import run_sequence
from host.sequence_session import run_session


PROFILE = dict(mv1=700, ccr1=48, mv2=900, ccr2=64, gain_ppm=1000000,
               offset_mv=0, boot_min_mv=600, boot_max_mv=1000,
               load_min_mv=600, load_max_mv=1000)
PLAN = {"background": {"ccr": 60}, "steps": [
    {"target": {"ccr": 2}, "duration_ns": 14000},
    {"target": {"ccr": 48}, "duration_ns": 30000000}],
    "post": {"ccr": 60}}


class FakeClient:
    def __init__(self, fail_start=False):
        self.verbs = []
        self.fail_start = fail_start
        self.armed = False
        self.started = False
        self.last_id = 0

    def exchange(self, verb):
        self.verbs.append(verb)
        if verb == "TELEM":
            return "OK TELEM state=PREARM source_ready=1"
        if verb == "STOP":
            return "OK STOP PASSIVE"
        raise AssertionError(verb)

    def command(self, verb):
        self.verbs.append(verb)
        if verb == "STATUS":
            return "OK STATUS ARMED fault=0" if self.armed else "OK STATUS PASSIVE fault=0"
        if verb == "SEQ STATUS":
            if self.started:
                return f"OK SEQ_STATUS id={self.last_id} active=1 running=0 feedback=1 run=1"
            return "OK SEQ_STATUS id=0 active=1 running=0 feedback=1 run=1"
        if verb.startswith("SEQ START"):
            self.started = True
            self.last_id = int(verb.split()[-1])
            if self.fail_start:
                raise ProtocolError("Missing/partial response; command is NOT retried")
            return f"OK SEQ_START id={self.last_id} period_hi=0 period_lo=129"
        if verb.startswith("ARM "):
            self.armed = True
            return "OK ARM_PENDING"
        return "OK " + verb.split()[0]

    def stop(self):
        return self.exchange("STOP")


class SequenceRunTests(unittest.TestCase):
    def test_upload_start_verify_and_stop(self):
        c = FakeClient()
        run_sequence(c, PROFILE, PLAN, sequence_id=7, lease_ms=300,
                     post_hold_s=0, qualification="FIXTURE_ISOLATED")
        self.assertIn("QUALIFY FIXTURE_ISOLATED", c.verbs)
        self.assertIn("SEQ COMMIT 7 2539706170", c.verbs)
        self.assertEqual(c.verbs.count("SEQ START 7"), 1)
        self.assertEqual(c.verbs[-1], "STOP")

    def test_lost_start_reply_never_retries(self):
        c = FakeClient(fail_start=True)
        with self.assertRaisesRegex(ProtocolError, "NOT retried"):
            run_sequence(c, PROFILE, PLAN, sequence_id=7, lease_ms=300,
                         post_hold_s=0, qualification="GPU_EXPERIMENT")
        self.assertEqual(c.verbs.count("SEQ START 7"), 1)
        self.assertEqual(c.verbs[-1], "STOP")

    def test_persistent_session_holds_post_between_two_plans(self):
        c = FakeClient()
        second = {"background": {"ccr": 60}, "steps": [
            {"target": {"ccr": 48}, "duration_ns": 30_000_000}],
            "post": {"ccr": 60}}
        run_session(c, PROFILE, PLAN, first_id=7, lease_ms=300,
                    qualification="FIXTURE_ISOLATED",
                    operations=[("idle", None, 0), ("status", None, 0),
                                ("plan", second, 8), ("stop", None, 0)])
        self.assertEqual(c.verbs.count("ARM 300"), 1)
        self.assertEqual(c.verbs.count("SEQ START 7"), 1)
        self.assertEqual(c.verbs.count("SEQ START 8"), 1)
        self.assertEqual(c.verbs.count("STOP"), 1)
        self.assertEqual(c.verbs[-1], "STOP")


if __name__ == "__main__":
    unittest.main()
