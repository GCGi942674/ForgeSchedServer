"""Offline tests for PJTest-style scheduled submissions."""
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest
from unittest import mock

import yaml

MODULE = Path(__file__).resolve().parents[2] / "server/clock.py"
spec = importlib.util.spec_from_file_location("forge_clock", str(MODULE))
clock = importlib.util.module_from_spec(spec)
spec.loader.exec_module(clock)


class ClockTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="forge-clock-")
        self.root = Path(self.temporary.name)
        self.work = self.root / "test2"
        case = self.work / "cases/smoke/run.tcl"
        case.parent.mkdir(parents=True)
        case.write_text("# fixture\n")
        self.runlist = self.work / "runlist"
        self.runlist.write_text(str(case) + "\n" + str(case) + "\n")
        self.templates = self.root / "templates"
        self.templates.mkdir()
        (self.templates / "route.json").write_text(json.dumps({
            "task_name": "route", "max_retry": 0,
            "flow_config": {"route_design": 1, "write_bitstream": 1}}))
        self.artifacts = self.root / "artifacts"
        self.artifacts.mkdir()
        (self.artifacts / "GalaxCore_18920.zip").write_bytes(b"fixture")
        self.yaml = self.root / "Tasks.yaml"
        self.yaml.write_text(yaml.safe_dump({"tasks": [{"template": "route",
            "target": str(self.runlist), "suite": "daily_regression"}]}))
        self.config = {"server.tasks_yaml": str(self.yaml),
                       "server.work_root": str(self.work),
                       "server.artifact_root": str(self.artifacts),
                       "server.templates_dir": str(self.templates),
                       "server.protect_versions_file": str(self.root / "protect_versions"),
                       "server.clock_state_file": str(self.root / "state.json"),
                       "network.server_ip": "127.0.0.1", "server.port": "18080"}

    def tearDown(self):
        self.temporary.cleanup()

    def test_plan_pins_version_and_flow(self):
        revision, archive, plan = clock.build_plan(self.config)
        self.assertEqual(revision, "18920")
        self.assertEqual(archive.name, "GalaxCore_18920.zip")
        self.assertEqual(len(plan), 1)
        self.assertEqual(plan[0]["payload"]["case"], "cases/smoke/run.tcl")
        self.assertEqual(plan[0]["payload"]["flow_config"]["route_design"], 1)
        self.assertEqual(plan[0]["payload"]["context"]["suite"], "daily_regression")

    def test_missing_template_blocks_whole_suite(self):
        self.yaml.write_text(yaml.safe_dump({"tasks": [
            {"template": "route", "target": str(self.runlist)},
            {"template": "missing", "target": str(self.runlist)}]}))
        with self.assertRaisesRegex(ValueError, "missing template"):
            clock.build_plan(self.config)
        self.assertFalse((self.root / "protect_versions").exists())

    def test_submit_once_and_preserve_uncertain_attempt(self):
        calls = []
        def reply(_, kind, data):
            calls.append(kind)
            if kind == "query_status":
                return {"pending": 0, "queued": 0, "assigned": 0, "running": 0, "total": 0}
            if kind == "query_task":
                return {"status": "SUCCEEDED"}
            self.assertEqual(data["payload"]["spec_version"], 2)
            return {"task_id": 101}
        with mock.patch.object(clock, "request", side_effect=reply):
            self.assertEqual(clock.run(self.config, interval=0), 0)
            self.assertEqual(clock.run(self.config, interval=0), 0)
        self.assertEqual(calls.count("submit_task"), 1)
        self.assertEqual((self.root / "protect_versions").read_text(), "18920\n")
        state = json.loads((self.root / "state.json").read_text())
        self.assertEqual(state["task_ids"], [101])
        self.assertEqual(state["phase"], "complete")

        (self.root / "state.json").unlink()
        def lost_ack(_, kind, data):
            if kind == "query_status":
                return {"pending": 0, "queued": 0, "assigned": 0, "running": 0, "total": 0}
            raise TimeoutError("ack lost")
        with mock.patch.object(clock, "request", side_effect=lost_ack):
            with self.assertRaises(TimeoutError):
                clock.run(self.config, interval=0)
            with self.assertRaisesRegex(RuntimeError, "uncertain"):
                clock.run(self.config, interval=0)
        self.assertTrue(json.loads((self.root / "state.json").read_text())["uncertain"])


if __name__ == "__main__":
    unittest.main()
