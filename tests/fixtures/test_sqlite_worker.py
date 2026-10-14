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
    with tempfile.TemporaryDirectory(prefix="gcrp-sqlite-worker-") as root:
        root = Path(root)
        with socket.socket() as probe:
            probe.bind(("127.0.0.1", 0))
            port = probe.getsockname()[1]
        config = root / "gcrp.conf"
        config.write_text("server.bind_ip=127.0.0.1\nserver.port={}\n"
                          "server.io_threads=2\nserver.worker_threads=2\n"
                          "network.server_ip=127.0.0.1\nlog.level=WARN\n"
                          "log.dir={}\ndatabase.path={}\nworker.output_dir={}\n"
                          "server.log_archive_root={}\n".format(
                              port, root / "logs", root / "state.db", root / "output", root / "archive"),
                          encoding="utf-8")
        env = os.environ.copy()
        env["GCRP_CONFIG"] = str(config)
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
            run_id = result["result"]["run_id"]
            assert run_id
            deadline = time.time() + 10
            while time.time() < deadline:
                listing = request(port, "query_logs", {"task_id": task_id, "run_id": run_id})
                assert listing["code"] == 0, listing
                files = {item["name"]: item for item in listing["result"]["items"]}
                if all(name in files and files[name]["complete"] for name in
                       ("stdout.log", "stderr.log", "result.json")):
                    break
                time.sleep(0.1)
            else:
                raise RuntimeError("worker logs timed out")
            page = request(port, "read_log", {"task_id": task_id, "run_id": run_id,
                           "name": "stdout.log", "offset": 0, "limit": 16384})
            assert page["code"] == 0, page
            assert b"demo stdout" in bytes.fromhex(page["result"]["data_hex"])
            denied = request(port, "read_log", {"task_id": task_id,
                             "run_id": "wrong-run", "name": "stdout.log",
                             "offset": 0, "limit": 16})
            assert denied["code"] != 0
            stop(worker)
            worker = subprocess.Popen([sys.executable, "-u", worker_script,
                                       "--worker-id", "p11-demo", "--demo",
                                       "--demo-seconds", "0.1", "--demo-exit-code", "7"],
                                      cwd=str(root), env=env,
                                      stdout=subprocess.PIPE, stderr=subprocess.PIPE)
            failed = request(port, "submit_task", {"task_type": "REGRESSION",
                "target": "demo-failure", "revision": "18925", "payload": {}})
            assert failed["code"] == 0, failed
            failed_id = failed["result"]["task_id"]
            deadline = time.time() + 15
            while time.time() < deadline:
                failed_query = request(port, "query_task", {"task_id": failed_id})
                if failed_query["result"]["status"] == "FAILED":
                    failed_run = failed_query["result"]["run_id"]
                    logs = request(port, "query_logs", {"task_id": failed_id,
                                    "run_id": failed_run})
                    files = {item["name"]: item for item in logs["result"]["items"]}
                    if "result.json" in files and files["result.json"]["complete"]:
                        break
                time.sleep(0.1)
            else:
                raise RuntimeError("failed Worker log upload timed out")
            stop(worker)
            worker = None
            stop(server)
            server = subprocess.Popen([server_bin], cwd=str(root), env=env,
                                      stdout=subprocess.PIPE, stderr=subprocess.PIPE)
            wait_server(port, server)
            restored = request(port, "query_task", {"task_id": task_id})
            assert restored["code"] == 0 and restored["result"]["status"] == "SUCCEEDED"
            archived = request(port, "query_logs", {"task_id": task_id, "run_id": run_id})
            assert archived["code"] == 0 and len(archived["result"]["items"]) >= 3
            replay = request(port, "submit_task", {"task_type": "REGRESSION",
                "target": "demo", "revision": "18925", "payload": {},
                "submission_key": "sqlite-worker-smoke"})
            assert replay["code"] == 0 and replay["result"]["task_id"] == task_id
        finally:
            stop(worker)
            stop(server)


if __name__ == "__main__":
    main()
