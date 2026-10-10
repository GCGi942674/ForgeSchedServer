#!/usr/bin/env python3
"""Single-slot Linux demo executor. Never executes commands from task metadata."""
import argparse
import hashlib
import json
import math
import os
from pathlib import Path
import re
import select
import signal
import socket
import struct
import subprocess
import sys
import tempfile
import time
from process_executor import ProcessExecutor
from pjtest_adapter import PJtestAdapter
from runtime_config import load_config

MAX_FRAME = 1024 * 1024
_SLOT_ID = re.compile(r"[A-Za-z0-9][A-Za-z0-9_-]{0,63}\Z")


def shared_pjtest_settings(config, worker_id, output):
    """Translate the common config into one dedicated SVN-backed slot."""
    artifact = config.get("worker.artifact_root", "")
    if not artifact:
        return None
    if not _SLOT_ID.fullmatch(worker_id):
        raise ValueError("PJtest worker-id must be 1-64 letters, digits, _ or -")
    slots_root = Path(config.get("worker.slots_root") or "./worker/workers_slots").expanduser().resolve()
    slot = slots_root / worker_id / "galaxcore"
    grace = float(config.get("worker.terminate_grace_seconds") or "30")
    clean_value = config.get("worker.clean") or "true"
    if clean_value not in ("true", "false"):
        raise ValueError("worker.clean must be true or false")
    return dict(galaxcore_root=str(slot), svn_url=config.get("worker.svn_url", ""),
                svn_revision=config.get("worker.svn_revision") or None,
                artifact_root=artifact, log_root=str(output), flow_profiles={},
                require_server_flow=True,
                clean=clean_value == "true", terminate_grace_seconds=grace)


class Worker:
    def __init__(self, args):
        self.args = args
        self.sock = None
        self.buffer = bytearray()
        self.sequence = 0
        self.assigned = None
        self.process = None
        self.executor = ProcessExecutor(
            lambda: self.request("worker_heartbeat", dict(worker_id=self.args.worker_id)))
        self.adapter = (PJtestAdapter(args.pjtest_config, args.worker_id, self.executor.heartbeat,
                                     args.pjtest_paths)
                        if args.pjtest_config else None)
        if self.adapter and not args.pjtest_paths.get("log_root"):
            args.output = self.adapter.log_root

    def pending_path(self):
        identity = hashlib.sha256(self.args.worker_id.encode("utf-8")).hexdigest()[:24]
        return self.args.output / ("pending-report-" + identity + ".json")

    def save_report(self, report):
        path = self.pending_path()
        fd, temporary = tempfile.mkstemp(prefix=".pending-", dir=str(self.args.output))
        try:
            with os.fdopen(fd, "w") as stream:
                json.dump(report, stream)
                stream.flush()
                os.fsync(stream.fileno())
            os.replace(temporary, str(path))
            directory_fd = os.open(str(self.args.output), os.O_RDONLY | os.O_DIRECTORY)
            try:
                os.fsync(directory_fd)
            finally:
                os.close(directory_fd)
        finally:
            if os.path.exists(temporary):
                os.unlink(temporary)

    def flush_report(self):
        path = self.pending_path()
        if not path.exists():
            return
        report = json.loads(path.read_text(encoding="utf-8"))
        if (not isinstance(report, dict) or report.get("worker_id") != self.args.worker_id or
                type(report.get("task_id")) is not int or
                report.get("status") not in ("SUCCEEDED", "FAILED", "TIMEOUT") or
                not isinstance(report.get("message"), str)):
            raise RuntimeError("invalid pending report; inspect " + str(path))
        self.request("task_result", report)
        path.unlink()
        directory_fd = os.open(str(self.args.output), os.O_RDONLY | os.O_DIRECTORY)
        try:
            os.fsync(directory_fd)
        finally:
            os.close(directory_fd)

    def receive(self, deadline):
        while True:
            if len(self.buffer) >= 4:
                size = struct.unpack("!I", self.buffer[:4])[0]
                if not 0 < size <= MAX_FRAME:
                    raise RuntimeError("invalid frame size")
                if len(self.buffer) >= size + 4:
                    value = json.loads(self.buffer[4:4 + size])
                    del self.buffer[:4 + size]
                    if (not isinstance(value, dict) or type(value.get("version")) is not int
                            or value["version"] != 1 or type(value.get("request_id")) is not int
                            or not 0 <= value["request_id"] < 2**64
                            or not isinstance(value.get("data"), dict)):
                        raise RuntimeError("invalid envelope")
                    return value
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise TimeoutError("server response timeout")
            if not select.select([self.sock], [], [], remaining)[0]:
                raise TimeoutError("server response timeout")
            chunk = self.sock.recv(65536)
            if not chunk:
                raise ConnectionError("server disconnected")
            self.buffer.extend(chunk)

    def assignment(self, message):
        data = message["data"]
        if (message.get("type") != "task_assign" or self.assigned is not None
                or type(data.get("task_id")) is not int or not 0 < data["task_id"] < 2**64
                or data.get("task_type") != "REGRESSION"
                or not isinstance(data.get("target"), str)
                or not isinstance(data.get("revision"), str)):
            raise RuntimeError("unexpected or excess assignment")
        self.assigned = data
        if not self.adapter and data.get("payload"):
            raise RuntimeError("demo Worker refuses business regression payload")

    def request(self, kind, data):
        self.sequence += 1
        body = json.dumps(dict(version=1, type=kind, request_id=self.sequence,
                               data=data)).encode()
        self.sock.sendall(struct.pack("!I", len(body)) + body)
        deadline = time.monotonic() + 5
        while True:
            message = self.receive(deadline)
            if message.get("type") == "task_assign":
                self.assignment(message)
                continue
            if (message.get("type") != "response" or message["request_id"] != self.sequence
                    or type(message["data"].get("code")) is not int
                    or message["data"]["code"] != 0):
                raise RuntimeError("request rejected: " + repr(message)[:500])
            return

    def stop_process(self):
        self.executor.stop()
        if self.process is not None:
            # A separate process group prevents orphan descendants on timeout/shutdown.
            try:
                os.killpg(self.process.pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
            self.process.wait()
            self.process = None

    def execute(self, root):
        assignment = self.assigned
        task_id = assignment["task_id"]
        self.request("task_start", dict(task_id=task_id, worker_id=self.args.worker_id))
        directory = root / str(task_id)
        if self.adapter:
            result = self.adapter.execute(assignment, directory)
            if not self.adapter.healthy:
                # Fail closed: no terminal report/released slot on cleanup failure.
                raise RuntimeError("PJtest cleanup failed; Worker quarantined; inspect local result.json")
        else:
            directory.mkdir()
            with (directory / "stdout.log").open("xb") as out, (directory / "stderr.log").open("xb") as err:
                script = ("import sys,time; print('demo stdout',flush=True); "
                          "print('demo stderr',file=sys.stderr,flush=True); "
                          "time.sleep(float(sys.argv[1])); sys.exit(int(sys.argv[2]))")
                pid, code, timed_out = self.executor.run(
                    [sys.executable, "-u", "-c", script, str(self.args.demo_seconds),
                     str(self.args.demo_exit_code)], directory, os.environ.copy(), out, err,
                    self.args.task_timeout, grace=0,
                    on_start=lambda child_pid: (directory / "pid").write_text(str(child_pid) + "\n"))
            result = dict(task_id=task_id, pid=pid, exit_code=(-signal.SIGKILL if timed_out else code),
                          status=("TIMEOUT" if timed_out else "SUCCEEDED" if code == 0 else "FAILED"))
            (directory / "result.json").write_text(json.dumps(result) + "\n")
        status = result["status"]
        report = dict(task_id=task_id, worker_id=self.args.worker_id,
                      status=status, message=json.dumps(result))
        self.save_report(report)
        # The server can send the next assignment before acknowledging this result.
        self.assigned = None
        self.flush_report()
        print(json.dumps(result), flush=True)

    def run(self):
        self.args.output.mkdir(parents=True, exist_ok=True)
        if self.pending_path().exists() and self.pending_path().is_symlink():
            raise RuntimeError("unsafe pending report path")
        root = Path(tempfile.mkdtemp(prefix="run-", dir=str(self.args.output)))
        try:
            self.sock = socket.create_connection((self.args.host, self.args.port), timeout=5)
            self.request("worker_register", dict(worker_id=self.args.worker_id,
                                                 hostname=socket.gethostname(), slots=1))
            self.flush_report()
            while True:
                if self.assigned is not None:
                    self.execute(root)
                else:
                    try:
                        self.assignment(self.receive(time.monotonic() + 1))
                    except TimeoutError:
                        self.request("worker_heartbeat", dict(worker_id=self.args.worker_id))
        finally:
            self.stop_process()
            if self.sock is not None:
                self.sock.close()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", help="override network.server_ip")
    parser.add_argument("--port", type=int, help="override server.port")
    parser.add_argument("--worker-id", required=True)
    parser.add_argument("--output", type=Path, help="override worker.output_dir")
    parser.add_argument("--demo-seconds", type=float, default=3)
    parser.add_argument("--demo-exit-code", type=int, default=0)
    parser.add_argument("--task-timeout", type=float, default=30)
    parser.add_argument("--pjtest-config", type=Path,
                        help="enable single-case PJtest mode using this local JSON config")
    parser.add_argument("--demo", action="store_true", help="explicitly enable non-business demo mode")
    args = parser.parse_args()
    try:
        config = load_config()
        explicit_output = args.output is not None
        args.host = args.host or config["network.server_ip"]
        args.port = args.port if args.port is not None else int(config["server.port"])
        args.output = args.output or Path(config["worker.output_dir"])
        explicit_config = args.pjtest_config is not None
        if not explicit_config and not args.demo:
            args.pjtest_config = shared_pjtest_settings(config, args.worker_id, args.output)
        args.pjtest_paths = {}
        if explicit_config:
            if config.get("worker.test2_root"):
                args.pjtest_paths["test2_root"] = config["worker.test2_root"]
            for key in ("galaxcore_root", "svn_url", "svn_revision"):
                if config.get("worker." + key):
                    args.pjtest_paths[key] = config["worker." + key]
            if config.get("worker.artifact_root"):
                args.pjtest_paths["artifact_root"] = config["worker.artifact_root"]
        if explicit_output and explicit_config:
            args.pjtest_paths["log_root"] = str(args.output)
    except (OSError, ValueError, KeyError) as error:
        parser.error("invalid shared config: " + str(error))
    if bool(args.pjtest_config) == args.demo:
        parser.error("choose exactly one of --pjtest-config or --demo")
    if (not 1 <= args.port <= 65535 or not 1 <= len(args.worker_id.encode()) <= 256
            or not math.isfinite(args.demo_seconds) or args.demo_seconds < 0
            or not math.isfinite(args.task_timeout) or args.task_timeout <= 0
            or not 0 <= args.demo_exit_code <= 255):
        parser.error("invalid port, worker identity, duration or exit code")
    def terminate(signum, frame):
        raise KeyboardInterrupt()
    signal.signal(signal.SIGTERM, terminate)
    try:
        Worker(args).run()
    except KeyboardInterrupt:
        return 0
    except (OSError, ValueError, RuntimeError) as error:
        print("[WORKER] " + str(error), file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
