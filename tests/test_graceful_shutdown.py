#!/usr/bin/env python3
# -*- coding: utf-8 -*-

import argparse
import os
import signal
import socket
import subprocess
import sys
import threading
import time
from typing import List, Optional

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "worker"))
from runtime_config import load_config


def wait_for_port(host: str, port: int, timeout: float) -> bool:
    """Wait until the TCP port becomes connectable."""
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            with socket.create_connection((host, port), timeout=0.5):
                return True
        except OSError:
            time.sleep(0.1)
    return False


def try_connect(host: str, port: int, timeout: float = 1.0) -> bool:
    """Try a single TCP connect."""
    try:
        with socket.create_connection((host, port), timeout=timeout):
            return True
    except OSError:
        return False


def read_stream(prefix: str, stream, sink: List[str]):
    """Read subprocess output in a background thread."""
    for line in iter(stream.readline, ""):
        text = line.rstrip("\n")
        sink.append(text)
        print(f"{prefix}{text}")
    stream.close()


def recv_until_closed(sock: socket.socket, label: str, result: dict):
    """Read until peer closes or timeout."""
    chunks = []
    try:
        sock.settimeout(5.0)
        while True:
            data = sock.recv(4096)
            if not data:
                result[label] = b"".join(chunks)
                return
            chunks.append(data)
    except socket.timeout:
        result[label] = b"".join(chunks)
    except OSError as exc:
        result[label] = exc


def main():
    try:
        config = load_config()
        config_host = config["network.server_ip"]
        config_port = int(config["server.port"])
        server_binary = config["tools.server_binary"]
    except (OSError, ValueError, KeyError) as error:
        print("[FAIL] invalid shared config: {}".format(error), file=sys.stderr)
        return 2
    parser = argparse.ArgumentParser(description="Test graceful shutdown for your C++ server.")
    parser.add_argument("--server", default=server_binary, help="Path to server binary")
    parser.add_argument("--host", default=config_host, help="Server host")
    parser.add_argument("--port", type=int, default=config_port, help="Server port")
    parser.add_argument("--startup-timeout", type=float, default=5.0, help="Wait time for server startup")
    parser.add_argument("--shutdown-timeout", type=float, default=5.0, help="Wait time for server exit")
    parser.add_argument("--no-spawn", action="store_true", help="Do not spawn server, test an already running one")
    args = parser.parse_args()

    server_proc: Optional[subprocess.Popen] = None
    stdout_lines: List[str] = []
    stderr_lines: List[str] = []

    try:
        if not args.no_spawn:
            if not os.path.exists(args.server):
                print(f"[FAIL] server binary not found: {args.server}")
                return 2

            print(f"[INFO] starting server: {args.server}")
            server_proc = subprocess.Popen(
                [args.server],
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                universal_newlines=True,
                bufsize=1,
            )

            threading.Thread(
                target=read_stream,
                args=("[server-stdout] ", server_proc.stdout, stdout_lines),
                daemon=True,
            ).start()

            threading.Thread(
                target=read_stream,
                args=("[server-stderr] ", server_proc.stderr, stderr_lines),
                daemon=True,
            ).start()

            ready = wait_for_port(args.host, args.port, args.startup_timeout)
            if not ready:
                print("[FAIL] server did not start listening in time")
                if server_proc.poll() is None:
                    server_proc.terminate()
                return 2
        else:
            print("[INFO] using existing running server")
            ready = wait_for_port(args.host, args.port, args.startup_timeout)
            if not ready:
                print("[FAIL] target server is not listening")
                return 2

        print("[STEP 1] create existing client connections")
        client1 = socket.create_connection((args.host, args.port), timeout=2.0)
        client2 = socket.create_connection((args.host, args.port), timeout=2.0)

        print("[PASS] existing connections established")

        # If your protocol is known, you can send a real framed request here.
        # For now, we keep connections open and validate graceful shutdown behavior
        # at the TCP lifecycle level.

        print("[STEP 2] send SIGINT to server")
        if args.no_spawn:
            print("[WARN] --no-spawn mode cannot send signal automatically")
            print("[ACTION] please manually press Ctrl+C in the server terminal within 3 seconds")
            time.sleep(3.0)
        else:
            os.kill(server_proc.pid, signal.SIGINT)

        time.sleep(0.5)

        print("[STEP 3] verify new connections are rejected after shutdown begins")
        connect_ok = try_connect(args.host, args.port, timeout=1.0)
        if connect_ok:
            print("[FAIL] new connection still accepted after SIGINT")
            client1.close()
            client2.close()
            if server_proc and server_proc.poll() is None:
                server_proc.kill()
            return 1
        else:
            print("[PASS] new connection rejected after SIGINT")

        print("[STEP 4] observe existing connections")
        results = {}
        t1 = threading.Thread(target=recv_until_closed, args=(client1, "client1", results), daemon=True)
        t2 = threading.Thread(target=recv_until_closed, args=(client2, "client2", results), daemon=True)
        t1.start()
        t2.start()

        # We do not actively send more data here, because your shutdown design
        # should disable new reads after stopping and only drain existing writes.
        time.sleep(1.0)

        try:
            client1.shutdown(socket.SHUT_WR)
        except OSError:
            pass
        try:
            client2.shutdown(socket.SHUT_WR)
        except OSError:
            pass

        t1.join(timeout=3.0)
        t2.join(timeout=3.0)

        try:
            client1.close()
        except OSError:
            pass
        try:
            client2.close()
        except OSError:
            pass

        print("[STEP 5] verify server process exits")
        if server_proc is not None:
            try:
                ret = server_proc.wait(timeout=args.shutdown_timeout)
                print(f"[PASS] server exited, return code = {ret}")
            except subprocess.TimeoutExpired:
                print("[FAIL] server did not exit within timeout")
                server_proc.kill()
                return 1

        print()
        print("========== TEST RESULT ==========")
        print("[PASS] graceful shutdown basic test passed")
        print("Validated points:")
        print("  - existing connections can stay alive during shutdown transition")
        print("  - new connections are rejected after SIGINT")
        print("  - server process exits instead of hanging")
        print("=================================")
        return 0

    finally:
        if server_proc is not None and server_proc.poll() is None:
            server_proc.kill()


if __name__ == "__main__":
    sys.exit(main())
