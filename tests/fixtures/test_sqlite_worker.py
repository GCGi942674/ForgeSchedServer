#!/usr/bin/env python3
"""Persistent Server -> Python demo Worker -> restart smoke test."""
import json
import os
from pathlib import Path
import socket
import struct
import subprocess
import sys
import tempfile
import time


def request(port, kind, data):
    body = json.dumps({"version": 1, "type": kind, "request_id": 1,
                       "data": data}).encode("utf-8")
    with socket.create_connection(("127.0.0.1", port), timeout=3) as conn:
        conn.sendall(struct.pack("!I", len(body)) + body)
        header = bytearray()
        while len(header) < 4:
            chunk = conn.recv(4 - len(header))
            if not chunk:
                raise RuntimeError("missing response header")
            header.extend(chunk)
        size = struct.unpack("!I", header)[0]
        if not 0 < size < 1024 * 1024:
            raise RuntimeError("invalid response size")
        result = bytearray()
        while len(result) < size:
            chunk = conn.recv(size - len(result))
            if not chunk:
                raise RuntimeError("truncated response")
            result.extend(chunk)
        return json.loads(result.decode("utf-8"))["data"]


def wait_server(port, process):
    deadline = time.time() + 10
    while time.time() < deadline:
        if process.poll() is not None:
            raise RuntimeError("server exited early")
        try:
            with socket.create_connection(("127.0.0.1", port), timeout=0.2):
                return
        except OSError:
            time.sleep(0.05)
    raise RuntimeError("server did not listen")


def stop(process):
    if process is None:
        return
    if process.poll() is None:
        process.terminate()
        try:
            process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait(timeout=5)


def main():
    server_bin, worker_script = sys.argv[1:3]
    with tempfile.TemporaryDirectory(prefix="forgesched-sqlite-worker-") as root:
        root = Path(root)
        with socket.socket() as probe:
            probe.bind(("127.0.0.1", 0))
            port = probe.getsockname()[1]
        config = root / "forgesched.conf"
        config.write_text("server.bind_ip=127.0.0.1\nserver.port={}\n"
                          "server.io_threads=2\nserver.worker_threads=2\n"
                          "network.server_ip=127.0.0.1\nlog.level=WARN\n"
                          "log.dir={}\ndatabase.path={}\nworker.output_dir={}\n".format(
                              port, root / "logs", root / "state.db", root / "output"),
                          encoding="utf-8")
        env = os.environ.copy()
        env["FORGESCHED_CONFIG"] = str(config)
        server = None
        worker = None
        try:
            server = subprocess.Popen([server_bin], cwd=str(root), env=env,
                                      stdout=subprocess.PIPE, stderr=subprocess.PIPE)
            wait_server(port, server)
            worker = subprocess.Popen([sys.executable, "-u", worker_script,
                                       "--worker-id", "p11-demo", "--demo",
                                       "--demo-seconds", "0.1", "--task-timeout", "5"],
                                      cwd=str(root), env=env,
                                      stdout=subprocess.PIPE, stderr=subprocess.PIPE)
            submitted = request(port, "submit_task", {"task_type": "REGRESSION",
                "target": "demo", "revision": "18925", "payload": {},
                "submission_key": "sqlite-worker-smoke"})
            assert submitted["code"] == 0, submitted
            task_id = submitted["result"]["task_id"]
            deadline = time.time() + 15
            while time.time() < deadline:
                result = request(port, "query_task", {"task_id": task_id})
                if result["result"]["status"] == "SUCCEEDED":
                    assert result["result"]["execution"]["exit_code"] == 0
                    break
                if worker.poll() is not None:
                    raise RuntimeError("worker exited: " + worker.stderr.read().decode("utf-8"))
                time.sleep(0.1)
            else:
                raise RuntimeError("worker result timed out")
            stop(worker)
            worker = None
            stop(server)
            server = subprocess.Popen([server_bin], cwd=str(root), env=env,
                                      stdout=subprocess.PIPE, stderr=subprocess.PIPE)
            wait_server(port, server)
            restored = request(port, "query_task", {"task_id": task_id})
            assert restored["code"] == 0 and restored["result"]["status"] == "SUCCEEDED"
            replay = request(port, "submit_task", {"task_type": "REGRESSION",
                "target": "demo", "revision": "18925", "payload": {},
                "submission_key": "sqlite-worker-smoke"})
            assert replay["code"] == 0 and replay["result"]["task_id"] == task_id
        finally:
            stop(worker)
            stop(server)


if __name__ == "__main__":
    main()
