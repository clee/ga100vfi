"""Offline contract tests; mocked API calls never touch the board."""
from contextlib import redirect_stdout
import importlib.machinery
import io
import json
from pathlib import Path
import tempfile
import types
import unittest
from unittest.mock import patch


ROOT = Path(__file__).resolve().parents[1]
module = types.ModuleType("pwmvidctl_under_test")
module.__file__ = str(ROOT / "bin/pwmvidctl")
importlib.machinery.SourceFileLoader(module.__name__, module.__file__).exec_module(module)
PROFILE = json.loads((ROOT / "profiles/gpu0-operational.json").read_text())


def args(*parts):
    return module.parser().parse_args(["--profile", str(ROOT / "profiles/gpu0-operational.json"), *parts])


class OperatorCliTests(unittest.TestCase):
    def test_authorized_linear_extrapolation_maps_voltage_jog(self):
        self.assertEqual((PROFILE["mv1"], PROFILE["ccr1"], PROFILE["mv2"], PROFILE["ccr2"]),
                         (600, 69, 1000, 168))
        for target_mv, expected_ccr in ((600, 69), (700, 94), (750, 106), (800, 119),
                                        (850, 131), (900, 143), (1000, 168)):
            spec = {"background": {"mv": target_mv},
                    "steps": [{"target": {"mv": target_mv}, "duration_ns": 10_000_000}],
                    "post": {"mv": target_mv}}
            plan, _ = module.prepare_sequence(PROFILE, spec, 1)
            self.assertEqual((plan["background_ccr"], plan["steps"][0]["ccr"],
                              plan["post_ccr"]), (expected_ccr,) * 3)

    def test_offline_plan_uses_real_host_validator_without_token_or_api(self):
        for name in ("gpu0-small-step.json", "gpu0-short-step.json"):
            with patch.object(module, "api_request", side_effect=AssertionError("online request")):
                output = io.StringIO()
                with redirect_stdout(output):
                    module.command(args("plan", str(ROOT / "examples" / name)))
                compiled = json.loads(output.getvalue())
            self.assertEqual(compiled["background_ccr"], 155)
            self.assertEqual(compiled["post_ccr"], 155)
            self.assertTrue(all(129 <= step["ccr"] <= 155 for step in compiled["steps"]))
        self.assertEqual(compiled["steps"][1]["feedback"], "unverified_short")

    def test_one_percent_example_is_exactly_two_timer_ticks(self):
        spec = json.loads((ROOT / "examples/gpu0-1v-1pct-vboot.json").read_text())
        plan, _ = module.prepare_sequence(PROFILE, spec, 1)
        self.assertEqual([step["ccr"] for step in plan["steps"]], [69, 168, 2, 168])
        self.assertEqual(plan["steps"][2]["periods"], 67_500)
        self.assertEqual(plan["steps"][2]["feedback"], "experimental_rail_only")
        self.assertEqual(plan["total_periods"], 472_500)

    def test_arm_and_sequence_require_explicit_flags_before_api(self):
        with patch.object(module, "api_request", side_effect=AssertionError("online request")):
            with self.assertRaisesRegex(module.OperatorError, "confirm-wiring"):
                module.command(args("arm", "--mode", "GPU_EXPERIMENT"))
            with self.assertRaisesRegex(module.OperatorError, "confirm-drive"):
                module.command(args("sequence", str(ROOT / "examples/gpu0-small-step.json")))

    def test_arm_posts_once_after_live_preflight(self):
        snap = {"connected": True, "armed": False, "profile_loaded": True,
                "profile": PROFILE, "telemetry": {"state": "PASSIVE", "run": "0", "break": "0",
                "gpu_ok": "1", "gpu_p": "238", "gpu_h": "153",
                "adc_ok": "1", "rail_mv": "950"}}
        calls = []
        def fake(_, path, data=None):
            calls.append((path, data))
            if path == "state": return snap
            if path == "arm": return {"job_id": "abc"}
            if path == "jobs/abc": return {"id": "abc", "state": "completed"}
            raise AssertionError(path)
        with patch.object(module, "api_request", side_effect=fake), redirect_stdout(io.StringIO()):
            module.command(args("arm", "--mode", "GPU_EXPERIMENT", "--confirm-wiring"))
        self.assertEqual([call for call in calls if call[0] == "arm"],
                         [("arm", {"mode": "GPU_EXPERIMENT"})])
        snap["telemetry"]["rail_mv"] = "99"
        with patch.object(module, "api_request", side_effect=fake):
            with self.assertRaisesRegex(module.OperatorError, "outside boot bounds"):
                module.command(args("arm", "--mode", "GPU_EXPERIMENT", "--confirm-wiring"))
        self.assertEqual(sum(path == "arm" for path, _ in calls), 1)

    def test_installed_rail_limits_are_inclusive_without_expanding_targets(self):
        snap = {"connected": True, "armed": False, "profile_loaded": True,
                "profile": PROFILE, "telemetry": {"state": "PASSIVE", "run": "0",
                "break": "0", "gpu_ok": "0", "adc_ok": "1", "rail_mv": "100"}}
        self.assertEqual((PROFILE["boot_min_mv"], PROFILE["boot_max_mv"],
                          PROFILE["load_min_mv"], PROFILE["load_max_mv"]),
                         (100, 1100, 100, 1100))
        for rail_mv in (100, 1100):
            snap["telemetry"]["rail_mv"] = str(rail_mv)
            module.ensure_arm_ready(snap, PROFILE, mode="GPU_STANDALONE", start_mv=900)
        for rail_mv in (99, 1101):
            snap["telemetry"]["rail_mv"] = str(rail_mv)
            with self.assertRaisesRegex(module.OperatorError, "outside boot bounds"):
                module.ensure_arm_ready(snap, PROFILE, mode="GPU_STANDALONE", start_mv=900)
        snap["telemetry"]["rail_mv"] = "800"
        for target_mv in (100, 1100):
            with self.assertRaisesRegex(module.OperatorError, "outside the installed base calibration"):
                module.ensure_arm_ready(snap, PROFILE, mode="GPU_STANDALONE", start_mv=target_mv)

    def test_standalone_arm_needs_rail_and_target_but_no_gpu_capture(self):
        snap = {"connected": True, "armed": False, "profile_loaded": True,
                "profile": PROFILE, "telemetry": {"state": "PASSIVE", "run": "0",
                "break": "0", "gpu_ok": "0", "adc_ok": "1", "rail_mv": "900"}}
        calls = []
        def fake(_, path, data=None):
            calls.append((path, data))
            if path == "state": return snap
            if path == "arm": return {"job_id": "abc"}
            if path == "jobs/abc": return {"id": "abc", "state": "completed"}
            raise AssertionError(path)
        with patch.object(module, "api_request", side_effect=fake), redirect_stdout(io.StringIO()):
            module.command(args("arm", "--mode", "GPU_STANDALONE", "--start-mv", "900", "--confirm-wiring"))
        self.assertEqual([call for call in calls if call[0] == "arm"],
                         [("arm", {"mode": "GPU_STANDALONE", "start_mv": 900})])
        with patch.object(module, "api_request", side_effect=fake):
            with self.assertRaisesRegex(module.OperatorError, "needs exactly one"):
                module.command(args("arm", "--mode", "GPU_STANDALONE", "--confirm-wiring"))
        self.assertEqual(sum(path == "arm" for path, _ in calls), 1)

    def test_background_mv_is_sent_once_only_when_armed(self):
        snap = {"connected": True, "armed": True, "profile_loaded": True,
                "profile": PROFILE, "telemetry": {"state": "ARMED", "run": "1"}}
        calls = []
        def fake(_, path, data=None):
            calls.append((path, data))
            if path == "state": return snap
            if path == "background": return {"job_id": "bg"}
            if path == "jobs/bg": return {"id": "bg", "state": "completed"}
            raise AssertionError(path)
        with patch.object(module, "api_request", side_effect=fake), redirect_stdout(io.StringIO()):
            module.command(args("background", "--mv", "900", "--confirm-drive"))
        self.assertEqual([item for item in calls if item[0] == "background"],
                         [("background", {"mv": 900})])
        snap["armed"] = False
        with patch.object(module, "api_request", side_effect=fake):
            with self.assertRaisesRegex(module.OperatorError, "explicitly ARMED"):
                module.command(args("background", "--mv", "900", "--confirm-drive"))

    def test_sequence_posts_once_only_when_armed(self):
        snap = {"connected": True, "armed": True, "profile_loaded": True,
                "profile": PROFILE, "telemetry": {"state": "ARMED", "run": "1"}}
        calls = []
        def fake(_, path, data=None):
            calls.append((path, data))
            if path == "state": return snap
            if path == "sequence": return {"job_id": "seq1"}
            if path == "jobs/seq1": return {"id": "seq1", "state": "completed"}
            raise AssertionError(path)
        with patch.object(module, "api_request", side_effect=fake), redirect_stdout(io.StringIO()):
            module.command(args("sequence", str(ROOT / "examples/gpu0-small-step.json"), "--confirm-drive"))
        self.assertEqual(sum(path == "sequence" for path, _ in calls), 1)
        self.assertEqual(calls[1][1]["plan"]["post"], {"ccr": 155})
        snap["armed"] = False
        with patch.object(module, "api_request", side_effect=fake):
            with self.assertRaisesRegex(module.OperatorError, "explicitly ARMED"):
                module.command(args("sequence", str(ROOT / "examples/gpu0-small-step.json"), "--confirm-drive"))
        self.assertEqual(sum(path == "sequence" for path, _ in calls), 1)

    def test_sequence_fast_capture_saves_private_result(self):
        snap = {"connected": True, "armed": True, "profile_loaded": True,
                "profile": PROFILE, "telemetry": {"state": "ARMED", "run": "1"}}
        calls = []
        def fake(_, path, data=None):
            calls.append((path, data))
            if path == "state": return snap
            if path == "sequence": return {"job_id": "seqfast"}
            if path == "jobs/seqfast":
                return {"id": "seqfast", "state": "completed",
                        "result": {"fast_telemetry": {"samples": [{"rail_mv": 800}]}}}
            raise AssertionError(path)
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "slew.json"
            with patch.object(module, "api_request", side_effect=fake), redirect_stdout(io.StringIO()):
                module.command(args("sequence", str(ROOT / "examples/gpu0-slew-600-900.json"),
                                    "--confirm-drive", "--capture-output", str(path)))
            self.assertEqual(json.loads(path.read_text())["job"]["result"]["fast_telemetry"]["samples"][0]["rail_mv"], 800)
            self.assertEqual(path.stat().st_mode & 0o777, 0o600)
            self.assertEqual(calls[1][1]["capture_telemetry"], True)

    def test_adc_trace_requires_capture_file_and_passes_threshold_once(self):
        with patch.object(module, "api_request", side_effect=AssertionError("online request")):
            with self.assertRaisesRegex(module.OperatorError, "capture-output"):
                module.command(args("sequence", str(ROOT / "examples/gpu0-slew-600-900.json"),
                                    "--confirm-drive", "--adc-trace-threshold-mv", "750"))
        snap = {"connected": True, "armed": True, "profile_loaded": True,
                "profile": PROFILE, "telemetry": {"state": "ARMED", "run": "1"}}
        calls = []
        def fake(_, path, data=None):
            calls.append((path, data))
            if path == "state": return snap
            if path == "sequence": return {"job_id": "trace"}
            if path == "jobs/trace":
                return {"id": "trace", "state": "completed", "result": {
                    "fast_telemetry": {"samples": []},
                    "adc_trace": {"status": {"cycles_per_scan": "4100"},
                                  "frames": [[], [], [], []], "complete": True}}}
            raise AssertionError(path)
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "trace.json"
            with patch.object(module, "api_request", side_effect=fake), redirect_stdout(io.StringIO()):
                module.command(args("sequence", str(ROOT / "examples/gpu0-slew-600-900.json"),
                                    "--confirm-drive", "--capture-output", str(path),
                                    "--adc-trace-threshold-mv", "750"))
            self.assertEqual(calls[1][1]["adc_trace_threshold_mv"], 750)
            self.assertTrue(json.loads(path.read_text())["job"]["result"]["adc_trace"]["complete"])

    def test_non_loopback_api_is_rejected_before_token_read(self):
        with tempfile.TemporaryDirectory() as temp:
            parsed = module.parser().parse_args(["--api-url", "http://remote.example:4183",
                "--token-file", str(Path(temp) / "nonexistent"), "status"])
            with self.assertRaisesRegex(module.OperatorError, "loopback"):
                module.command(parsed)

    def test_export_does_not_overwrite_an_existing_artifact(self):
        with tempfile.TemporaryDirectory() as temp:
            path = Path(temp) / "recording.json"
            path.write_text("existing")
            with self.assertRaisesRegex(module.OperatorError, "already exists"):
                module.write_private_json(path, {"observations": []})
            self.assertEqual(path.read_text(), "existing")


if __name__ == "__main__":
    unittest.main()
