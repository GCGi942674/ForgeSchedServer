"""Offline adversarial tests invoked by test_worker_safety.cpp."""
import fcntl
import json
import os
from pathlib import Path
import shutil
import signal
import subprocess
import sys
import tempfile
import time
import unittest
import zipfile

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "worker"))
from pjtest_adapter import PJtestAdapter, AdapterError, tree_digest, file_digest
from process_executor import ProcessExecutor, ExecutionContainmentError
from forge_worker import Worker


def wait_for(predicate, seconds=8):
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        if predicate():
            return
        time.sleep(.02)
    raise AssertionError("condition timed out")


def gone(pid):
    try:
        return Path(f"/proc/{pid}/stat").read_text().rsplit(")", 1)[1].split()[0] == "Z"
    except FileNotFoundError:
        return True


class Safety(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix="forge-safety-")
        self.root = Path(self.tmp.name)
        subprocess.run([sys.executable, "-B", str(REPO / "tests/fixtures/pjtest/setup.py"),
                        str(self.root)], check=True, stdout=subprocess.DEVNULL)
        self.source = self.root / "slot/test2"
        self.archive = self.root / "artifacts/GalaxCore_58231.zip"
        self.config = self.root / "worker.json"
        self.task = dict(task_id=1, task_type="REGRESSION", revision="58231", target="xcvu9p",
                         payload=dict(spec_version=1, case="cases/success/run.tcl",
                                      flow="route", timeout_seconds=1))

    def tearDown(self):
        self.tmp.cleanup()

    def manifest(self):
        Path(str(self.archive) + ".manifest.json").write_text(json.dumps(dict(
            spec_version=1, revision="58231", test2_revision="fixture-1",
            archive_sha256=file_digest(self.archive), test2_sha256=tree_digest(self.source))))

    def adapter(self, name="one"):
        return PJtestAdapter(self.config, name, lambda: None)

    def run_adapter(self, adapter=None, name="result"):
        return (adapter or self.adapter()).execute(self.task, self.root / name)

    def test_private_workspace_and_environment(self):
        before = tree_digest(self.source)
        old = os.environ.get("GALAXCORE_WORKSPACE_ROOT")
        os.environ["GALAXCORE_WORKSPACE_ROOT"] = "/unrelated"
        try:
            result = self.run_adapter()
        finally:
            if old is None:
                os.environ.pop("GALAXCORE_WORKSPACE_ROOT")
            else:
                os.environ["GALAXCORE_WORKSPACE_ROOT"] = old
        self.assertEqual(result["status"], "SUCCEEDED")
        self.assertEqual(tree_digest(self.source), before)
        self.assertNotEqual(result["workspace"], str(self.source))
        self.assertTrue((self.root / "result/result.env").is_file())

    def test_canonical_lock_not_worker_identity(self):
        fd = os.open(self.source, os.O_RDONLY | os.O_DIRECTORY)
        try:
            fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
            self.assertEqual(self.run_adapter(self.adapter("different"))["reason"], "slot_busy")
        finally:
            os.close(fd)

    def test_ambiguous_artifact(self):
        shutil.copyfile(self.archive, self.archive.with_name("Galaxcore_58231.zip"))
        self.assertIn("ambiguous", self.run_adapter()["reason"])

    def test_missing_manifest(self):
        Path(str(self.archive) + ".manifest.json").unlink()
        self.assertEqual(self.run_adapter()["status"], "FAILED")

    def test_partial_or_stale_artifact(self):
        with self.archive.open("ab") as out:
            out.write(b"changed")
        self.assertEqual(self.run_adapter()["reason"], "artifact hash mismatch")

    def test_test2_changed(self):
        (self.source / "run.sh").write_text("exit 0\n")
        self.assertEqual(self.run_adapter()["reason"], "test2 hash mismatch")

    def test_symlink_inputs_rejected(self):
        (self.source / "link").symlink_to("/etc/passwd")
        self.assertIn("symlink", self.run_adapter()["reason"])

    def test_bad_archive_never_touches_shared_install(self):
        with zipfile.ZipFile(self.archive, "a") as archive:
            archive.writestr("../outside", "bad")
        self.manifest()
        self.assertIn("unsafe artifact", self.run_adapter()["reason"])
        self.assertFalse((self.root / "slot/bin").exists())
        self.assertFalse((self.root / "outside").exists())

    def test_no_old_flow_overlay(self):
        first = self.run_adapter()
        self.assertEqual(first["status"], "SUCCEEDED")
        with zipfile.ZipFile(self.archive, "w") as archive:
            archive.writestr("bin/Linux_64/GalaxCore", "#!/bin/sh\nexit 0\n")
            archive.writestr("flow/new.txt", "new")
        self.manifest()
        second = self.run_adapter(name="second")
        self.assertEqual(second["status"], "SUCCEEDED")
        installed = Path(second["workspace"]).parent / "flow"
        self.assertFalse((installed / "marker.txt").exists())
        self.assertTrue((installed / "new.txt").exists())

    def test_postclean_failure_quarantines(self):
        (self.source / "clean.sh").write_text(
            "#!/bin/bash\nif [ -e cleaned ]; then exit 9; fi\ntouch cleaned\n")
        self.manifest()
        adapter = self.adapter()
        result = self.run_adapter(adapter)
        self.assertEqual(result["reason"], "post_clean_failed")
        self.assertFalse(adapter.healthy)
        self.assertTrue((self.root / "result/result.env").exists())
        with self.assertRaises(AdapterError):
            self.run_adapter(adapter, "again")

    def test_foreign_namespace_not_accepted(self):
        script = self.source / "run.sh"
        script.write_text(script.read_text().replace("$VIVADO_RUNNER_NAMESPACE", "old-namespace"))
        self.manifest()
        self.assertEqual(self.run_adapter()["reason"], "missing_result")

    def test_coarse_timestamp_does_not_reject_own_result(self):
        script = self.source / "run.sh"
        script.write_text(script.read_text().replace(
            'if [ "$mode" = pass_nonzero ]', 'touch -d @1 "$status_dir/result.env"\nif [ "$mode" = pass_nonzero ]'))
        self.manifest()
        self.assertEqual(self.run_adapter()["status"], "SUCCEEDED")

    def test_demo_rejects_business_assignment(self):
        worker = Worker.__new__(Worker)
        worker.assigned, worker.adapter = None, None
        with self.assertRaisesRegex(RuntimeError, "refuses"):
            worker.assignment(dict(type="task_assign", data=self.task))

    def test_worker_requires_explicit_mode(self):
        result = subprocess.run([sys.executable, "-B", str(REPO / "worker/forge_worker.py"),
                                 "--worker-id", "no-mode"], capture_output=True)
        self.assertEqual(result.returncode, 2)

    def test_result_symlink_and_size(self):
        path = self.root / "result.env"
        path.symlink_to("/etc/passwd")
        with self.assertRaises(OSError):
            PJtestAdapter._parse(path, self.source / "cases/success/run.tcl")
        path.unlink()
        path.write_bytes(b"x" * 65537)
        with self.assertRaises(AdapterError):
            PJtestAdapter._parse(path, self.source / "cases/success/run.tcl")

    @unittest.skipUnless((REPO / "reference/test2/run.sh").is_file(), "optional local reference not installed")
    def test_real_runner_with_fake_binary(self):
        reference = REPO / "reference/test2"
        shutil.copyfile(reference / "run.sh", self.source / "run.sh")
        shutil.copyfile(reference / "clean.sh", self.source / "clean.sh")
        for name in ("lib", "config", "templates"):
            shutil.copytree(reference / "vivado_runner" / name, self.source / "vivado_runner" / name)
        (self.source / "flow_config").write_text(
            (reference / "vivado_runner/templates/flow_config.template").read_text())
        configuration = json.loads(self.config.read_text())
        configuration["flow_profiles"]["route"].update(
            write_bitstream=0, bit_cmp=0, msk_cmp=0, bgn_cmp=0)
        self.config.write_text(json.dumps(configuration))
        with zipfile.ZipFile(self.archive, "w") as archive:
            archive.writestr("bin/Linux_64/GalaxCore", "#!/bin/bash\nprintf 'Runtime: 1\\n'\n")
            archive.writestr("flow/marker.txt", "fake")
        self.manifest()
        self.task["payload"]["timeout_seconds"] = 10
        result = self.run_adapter()
        if result["status"] != "SUCCEEDED":
            print((self.root / "result/stdout.log").read_text())
            print((self.root / "result/stderr.log").read_text())
        self.assertEqual(result["status"], "SUCCEEDED", result)
        self.assertTrue((self.root / "result/result.env").is_file())
        self.assertEqual((self.root / "result/evidence/run").read_text().strip(), "Runtime: 1")

    def process(self, script, timeout=2, heartbeat=None):
        with (self.root / "process.log").open("wb") as log:
            return ProcessExecutor(heartbeat).run(
                [sys.executable, "-u", "-c", script], self.root, os.environ.copy(),
                log, log, timeout, grace=.1)

    def test_detached_double_fork_parent_exits_first(self):
        script = """import os,signal,time
if os.fork() == 0:
    os.setsid()
    if os.fork() == 0:
        signal.signal(signal.SIGTERM, signal.SIG_IGN)
        open('escaped.pid','w').write(str(os.getpid()))
        time.sleep(60)
    os._exit(0)
while not os.path.exists('escaped.pid'): time.sleep(.01)
os._exit(0)
"""
        _, code, timed_out = self.process(script)
        self.assertEqual(code, 125)
        self.assertFalse(timed_out)
        self.assertTrue(gone(int((self.root / "escaped.pid").read_text())))

    def test_timeout_reaps_term_ignoring_descendants(self):
        script = """import os,signal,time
if os.fork() == 0:
    os.setsid()
    signal.signal(signal.SIGTERM, signal.SIG_IGN)
    open('escaped.pid','w').write(str(os.getpid()))
    time.sleep(60)
time.sleep(60)
"""
        _, code, timed_out = self.process(script, .3)
        self.assertEqual(code, 124)
        self.assertTrue(timed_out)
        self.assertTrue(gone(int((self.root / "escaped.pid").read_text())))

    def test_heartbeat_failure_reaps(self):
        calls = [0]
        def heartbeat():
            calls[0] += 1
            if calls[0] > 1:
                raise ConnectionError("lost server")
        with self.assertRaises(ConnectionError):
            self.process("import os,time;open('child.pid','w').write(str(os.getpid()));time.sleep(60)",
                         heartbeat=heartbeat)
        self.assertTrue(gone(int((self.root / "child.pid").read_text())))

    def test_shutdown_immediately_after_spawn(self):
        for _ in range(5):
            executor = ProcessExecutor()
            with (self.root / "startup.log").open("wb") as log:
                def interrupted(_pid):
                    raise KeyboardInterrupt()
                with self.assertRaises(KeyboardInterrupt):
                    executor.run([sys.executable, "-c", "import time;time.sleep(60)"],
                                 self.root, os.environ.copy(), log, log, 60, .1, interrupted)
            self.assertTrue(executor.safe)

    def test_missing_supervisor_receipt_is_not_completion(self):
        executor = ProcessExecutor()
        with (self.root / "lost-guard.log").open("wb") as log:
            def kill_guard(pid):
                os.kill(pid, signal.SIGKILL)
            with self.assertRaises(ExecutionContainmentError):
                # Short-lived command avoids leaving any long-running test child
                # even if the guard starts it before the injected SIGKILL.
                executor.run([sys.executable, "-c", "pass"], self.root,
                             os.environ.copy(), log, log, 2, .1, kill_guard)
        self.assertFalse(executor.safe)

    def test_worker_sigkill_guard_survives_and_reaps(self):
        child_script = ("import os,signal,time;os.setsid();"
                        "signal.signal(signal.SIGTERM,signal.SIG_IGN);"
                        "open('escaped.pid','w').write(str(os.getpid()));time.sleep(60)")
        # The direct command is already a session leader; create an escaped child.
        command = "import subprocess,time;subprocess.Popen(" + repr(
            [sys.executable, "-c", child_script]) + ");time.sleep(60)"
        driver = ("import sys,os;sys.path.insert(0," + repr(str(REPO / "worker")) +
                  ");from process_executor import ProcessExecutor;"
                  "f=open('guard.log','wb');ProcessExecutor().run(" +
                  repr([sys.executable, "-c", command]) +
                  ",'.',os.environ.copy(),f,f,60,.1,"
                  "lambda pid:open('guard.pid','w').write(str(pid)))")
        process = subprocess.Popen([sys.executable, "-B", "-c", driver], cwd=self.root)
        try:
            wait_for(lambda: (self.root / "escaped.pid").exists())
            pid = int((self.root / "escaped.pid").read_text())
            process.kill()
            process.wait(timeout=3)
            wait_for(lambda: gone(pid))
            wait_for(lambda: gone(int((self.root / "guard.pid").read_text())))
        finally:
            if process.poll() is None:
                process.terminate()
                process.wait(timeout=5)


if __name__ == "__main__":
    unittest.main()
