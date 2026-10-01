import unittest
from host.sequence_plan import compile_plan, duty_to_ccr, upload_commands


class SequencePlanTests(unittest.TestCase):
    def test_sub_ten_microsecond_steps_are_pwm_boundary_quantized(self):
        spec = {"background": {"ccr": 60}, "steps": [
            {"target": {"ccr": 2}, "duration_ns": 1482},
            {"target": {"ccr": 60}, "duration_ns": 4444},
            {"target": {"ccr": 48}, "duration_ns": 8889},
        ], "post": {"ccr": 60}}
        plan = compile_plan(spec)
        self.assertEqual([s["periods"] for s in plan["steps"]], [1, 3, 6])
        self.assertEqual([s["end_period"] for s in plan["steps"]], [1, 4, 10])
        self.assertEqual(plan["total_periods"], 10)

    def test_absolute_rounding_avoids_segment_drift(self):
        spec = {"background": {"ccr": 60}, "steps": [
            {"target": {"ccr": 60}, "duration_ns": 1000},
            {"target": {"ccr": 61}, "duration_ns": 1000},
            {"target": {"ccr": 60}, "duration_ns": 1000},
        ], "post": {"ccr": 60}}
        with self.assertRaisesRegex(ValueError, "shorter than one"):
            compile_plan(spec)
        spec["steps"] = [
            {"target": {"ccr": 60}, "duration_ns": 2250},
            {"target": {"ccr": 61}, "duration_ns": 2250},
            {"target": {"ccr": 60}, "duration_ns": 2250},
        ]
        plan = compile_plan(spec)
        self.assertEqual([s["end_period"] for s in plan["steps"]], [2, 3, 5])

    def test_example_requires_expanded_envelope(self):
        import json
        from pathlib import Path
        spec = json.loads((Path(__file__).resolve().parents[1] / "host/sequence.example.json").read_text())
        with self.assertRaisesRegex(ValueError, "outside the selected 16..224 envelope"):
            compile_plan(spec, min_ccr=16)
        plan = compile_plan(spec)
        self.assertTrue(plan["experimental_low_duty"])
        self.assertEqual([s["ccr"] for s in plan["steps"]], [2, 60, 2, 48, 2])
        self.assertEqual(plan["background_ccr"], 60)
        self.assertEqual(plan["post_ccr"], 60)
        self.assertEqual(plan["steps"][2]["periods"], 10)
        self.assertTrue(all(abs(s["end_error_ns_numerator"]) * 2 <= 1_000_000_000
                            for s in plan["steps"]))
        self.assertEqual(plan["steps"][-1]["end_period"], plan["total_periods"])

    def test_rejects_invalid_values(self):
        self.assertEqual(duty_to_ccr("25"), 60)
        self.assertEqual(duty_to_ccr("1"), 2)
        for value in ("NaN", "Infinity", "-1", "101"):
            with self.assertRaises(ValueError):
                duty_to_ccr(value)
        spec = {"background": {"ccr": 60}, "steps": [
            {"target": {"ccr": 60}, "duration_ns": True}], "post": {"ccr": 60}}
        with self.assertRaisesRegex(ValueError, "integer"):
            compile_plan(spec)

    def test_feedback_policy_matches_cb_limits(self):
        spec = {"background": {"ccr": 60}, "steps": [
            {"target": {"ccr": 2}, "duration_ns": 500000000},
            {"target": {"ccr": 60}, "duration_ns": 20000000}],
            "post": {"ccr": 60}}
        self.assertEqual(compile_plan(spec)["steps"][0]["feedback"],
                         "experimental_rail_only")
        spec["steps"][0]["duration_ns"] = 500010000
        with self.assertRaisesRegex(ValueError, "500 ms"):
            compile_plan(spec)
        spec["steps"] = [{"target": {"ccr": 2}, "duration_ns": 4000000},
                         {"target": {"ccr": 60}, "duration_ns": 4000000},
                         {"target": {"ccr": 2}, "duration_ns": 4000000}]
        with self.assertRaisesRegex(ValueError, "10 ms"):
            compile_plan(spec)
        spec["background"] = {"ccr": 2}
        with self.assertRaisesRegex(ValueError, "background and post"):
            compile_plan(spec)

    def test_voltage_targets_use_measured_endpoints_in_both_directions(self):
        profile = dict(mv1=700, ccr1=48, mv2=900, ccr2=64,
                       gain_ppm=1000000, offset_mv=0,
                       boot_min_mv=600, boot_max_mv=1000,
                       load_min_mv=600, load_max_mv=1000)
        spec = {"background": {"mv": 850}, "steps": [
            {"target": {"mv": 800}, "duration_ns": 14_000}],
            "post": {"mv": 700}}
        plan = compile_plan(spec, profile=profile)
        self.assertEqual((plan["background_ccr"], plan["steps"][0]["ccr"],
                          plan["post_ccr"]), (60, 56, 48))
        profile["ccr1"], profile["ccr2"] = 64, 48
        plan = compile_plan(spec, profile=profile)
        self.assertEqual((plan["background_ccr"], plan["steps"][0]["ccr"],
                          plan["post_ccr"]), (52, 56, 64))
        with self.assertRaisesRegex(ValueError, "require a measured"):
            compile_plan(spec)
        spec["steps"][0]["target"] = {"mv": 901}
        with self.assertRaisesRegex(ValueError, "outside the measured"):
            compile_plan(spec, profile=profile)

    def test_proposed_upload_has_stable_cross_language_digest(self):
        import json
        from pathlib import Path
        spec = json.loads((Path(__file__).resolve().parents[1] / "host/sequence.example.json").read_text())
        plan = compile_plan(spec)
        commands = upload_commands(plan, 1)
        self.assertEqual(commands["digest_crc32"], 1555189105)
        self.assertEqual(commands["upload_verbs"][2], "SEQ ADD 1 0 337500 2")
        self.assertEqual(commands["upload_verbs"][-1], "SEQ COMMIT 1 1555189105")
        self.assertEqual(commands["start_verb"], "SEQ START 1")
        self.assertNotIn(commands["start_verb"], commands["upload_verbs"])
        self.assertNotEqual(upload_commands(plan, 2)["digest_crc32"], commands["digest_crc32"])
        plan["steps"][2]["ccr"] = 3
        self.assertNotEqual(upload_commands(plan, 1)["digest_crc32"], commands["digest_crc32"])
        with self.assertRaisesRegex(ValueError, "sequence_id"):
            upload_commands(plan, 0)


if __name__ == "__main__":
    unittest.main()
