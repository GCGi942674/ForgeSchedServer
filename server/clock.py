#!/usr/bin/env python3
"""Submit a PJTest-style Tasks.yaml suite to ForgeSched once per local day.

Invoke from cron or manually; this process does not stay resident.
"""
import argparse
from datetime import date
import fcntl
import json
import os
from pathlib import Path
import re
import socket
import struct
import sys
import tempfile
import time

try:
    import yaml
except ImportError:
    raise SystemExit("PyYAML is required for Tasks.yaml (python3 -m pip install PyYAML)")

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "worker"))
from runtime_config import load_config

MAX_FRAME = 1024 * 1024
ZIP_NAME = re.compile(r"Galax[Cc]ore_(\d+)\.zip\Z")
SAFE_NAME = re.compile(r"[A-Za-z0-9][A-Za-z0-9_-]{0,63}\Z")
SAFE_KEY = re.compile(r"[A-Za-z_][A-Za-z0-9_]{0,63}\Z")
TERMINAL = {"SUCCEEDED", "FAILED", "TIMEOUT", "CANCELLED"}


def required_path(config, key):
    value = config.get(key)
    if not value:
        raise ValueError("configure " + key)
    return Path(value).expanduser().resolve()


def normalize_revision(value):
    if value is None or str(value).strip().lower() == "latest" or str(value).strip() == "":
        return None
    text = str(value).strip()
    if not text.isdigit():
        raise ValueError("revision must be numeric or latest")
    return str(int(text))


def choose_revision(data, entries, artifact_root):
    fixed = {normalize_revision(entry.get("revision")) for entry in entries}
    fixed.discard(None)
    requested = normalize_revision(data.get("batch_revision", data.get("revision")))
    if requested is None and len(fixed) > 1:
        raise ValueError("Tasks.yaml has conflicting fixed revisions")
    requested = requested or (next(iter(fixed)) if fixed else None)
    available = {}
    for path in artifact_root.iterdir():
        match = ZIP_NAME.fullmatch(path.name)
        if match and path.is_file() and not path.is_symlink():
            available[int(match.group(1))] = path
    if not available:
        raise ValueError("no GalaxCore_N.zip in " + str(artifact_root))
    number = int(requested) if requested else max(available)
    if number not in available:
        raise ValueError("selected ZIP version {} is missing".format(number))
    return str(number), available[number]


def within(root, path):
    path = Path(path).resolve(strict=True)
    try:
        path.relative_to(root)
    except ValueError:
        raise ValueError("target escapes server.work_root: " + str(path))
    return path


def discover_cases(root, target):
    target = Path(target).expanduser()
    if target.is_absolute() and target.is_file():
        listing = within(root, target)
        paths = []
        for line in listing.read_text(encoding="utf-8").splitlines():
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            case = Path(line)
            if not case.is_absolute():
                raise ValueError("runlist entry must be an absolute run.tcl path")
            paths.append(within(root, case))
    else:
        if not target.is_absolute() and (target.is_file() or (root / target).is_file()):
            raise ValueError("relative runlist file is unsupported")
        directory = within(root, target if target.is_absolute() else root / target)
        if not directory.is_dir():
            raise ValueError("target must be a directory or an absolute runlist")
        paths = []
        for current, dirs, files in os.walk(str(directory)):
            dirs[:] = sorted(name for name in dirs if name not in
                             (".svn", ".git", "output", "outputs", "result", "results",
                              "report", "reports", "log", "logs", "__pycache__"))
            if "run.tcl" in files:
                paths.append(within(root, Path(current) / "run.tcl"))
    cases = sorted(set(path.relative_to(root).as_posix() for path in paths
                       if path.name == "run.tcl"))
    if not cases:
        raise ValueError("target has no run.tcl: " + str(target))
    if len(cases) > 100000:
        raise ValueError("too many cases in one suite")
    return cases


def load_template(directory, name):
    if not isinstance(name, str) or not SAFE_NAME.fullmatch(name):
        raise ValueError("invalid template name")
    path = directory / (name + ".json")
    if not path.is_file() or path.is_symlink():
        raise ValueError("missing template: " + str(path))
    if path.stat().st_size > 32768:
        raise ValueError("template is too large: " + str(path))
    template = json.loads(path.read_text(encoding="utf-8"))
    if not isinstance(template, dict):
        raise ValueError("template must be an object")
    flow = template.get("flow_config")
    if (not isinstance(flow, dict) or not 1 <= len(flow) <= 64 or
            any(not SAFE_KEY.fullmatch(key) or type(value) is not int or
                not 0 <= value <= 1000000 for key, value in flow.items())):
        raise ValueError("invalid template flow_config: " + str(path))
    return template


def build_plan(config):
    yaml_path = required_path(config, "server.tasks_yaml")
    root = required_path(config, "server.work_root")
    artifact_root = required_path(config, "server.artifact_root")
    templates = required_path(config, "server.templates_dir")
    source = yaml.safe_load(yaml_path.read_text(encoding="utf-8"))
    if isinstance(source, list):
        source = {"tasks": source}
    if not isinstance(source, dict) or not isinstance(source.get("tasks"), list) or not source["tasks"]:
        raise ValueError("Tasks.yaml must contain a nonempty tasks list")
    entries = source["tasks"]
    if any(not isinstance(entry, dict) or not entry.get("template") for entry in entries):
        raise ValueError("each task needs a template")
    revision, archive = choose_revision(source, entries, artifact_root)
    batch_id = date.today().isoformat() + "-" + revision
    plan = []
    for entry in entries:
        name = entry["template"]
        template = load_template(templates, name)
        retry = entry.get("max_retry", template.get("max_retry", 0))
        if type(retry) is not int or retry != 0:
            raise ValueError("max_retry requires recovery support; set it to 0 for " + name)
        priority = entry.get("priority", template.get("priority", 0))
        timeout = entry.get("max_time", template.get("max_time", 3600))
        if (type(priority) is not int or not -100 <= priority <= 100 or
                type(timeout) is not int or not 1 <= timeout <= 86400):
            raise ValueError("invalid priority or max_time for " + name)
        target = entry.get("target")
        if not isinstance(target, str) or not target:
            raise ValueError("task target is required for " + name)
        suite = str(entry.get("suite", template.get("suite", "")))
        task_name = str(entry.get("name", template.get("task_name", name)))
        if any(len(value) > 128 for value in (suite, task_name, batch_id)):
            raise ValueError("task context is too long")
        for case in discover_cases(root, target):
            plan.append({"task_type": "REGRESSION", "target": task_name,
                         "revision": revision, "priority": priority,
                         "payload": {"spec_version": 2, "case": case, "flow": name,
                                     "timeout_seconds": timeout,
                                     "flow_config": dict(template["flow_config"]),
                                     "context": {"template": name, "suite": suite,
                                                 "name": task_name, "batch_id": batch_id}}})
    if not plan:
        raise ValueError("empty task plan")
    return revision, archive, plan


def recv_exact(sock, size):
    data = bytearray()
    while len(data) < size:
        chunk = sock.recv(size - len(data))
        if not chunk:
            raise ConnectionError("ForgeSched connection closed")
        data.extend(chunk)
    return bytes(data)


def request(config, kind, data):
    host = config.get("network.server_ip")
    port = int(config.get("server.port", 0))
    timeout = float(config.get("network.timeout_ms", 5000)) / 1000
    body = json.dumps({"version": 1, "type": kind, "request_id": 1,
                       "data": data}, separators=(",", ":")).encode("utf-8")
    if len(body) > MAX_FRAME:
        raise ValueError("request exceeds protocol limit")
    with socket.create_connection((host, port), timeout=timeout) as sock:
        sock.settimeout(timeout)
        sock.sendall(struct.pack("!I", len(body)) + body)
        size = struct.unpack("!I", recv_exact(sock, 4))[0]
        if not 0 < size <= MAX_FRAME:
            raise ValueError("invalid ForgeSched response frame")
        reply = json.loads(recv_exact(sock, size).decode("utf-8"))
    if (not isinstance(reply, dict) or reply.get("type") != "response" or
            reply.get("request_id") != 1 or not isinstance(reply.get("data"), dict)):
        raise ValueError("invalid ForgeSched response")
    result = reply["data"]
    if result.get("code") != 0:
        raise RuntimeError("{} rejected: {}".format(kind, result.get("message")))
    return result.get("result")


def atomic_text(path, content):
    path.parent.mkdir(parents=True, exist_ok=True)
    fd, temporary = tempfile.mkstemp(prefix="." + path.name + "-", dir=str(path.parent))
    try:
        with os.fdopen(fd, "w") as stream:
            stream.write(content)
            stream.flush()
            os.fsync(stream.fileno())
        os.chmod(temporary, 0o644)
        os.replace(temporary, str(path))
        directory_fd = os.open(str(path.parent), os.O_RDONLY | os.O_DIRECTORY)
        try:
            os.fsync(directory_fd)
        finally:
            os.close(directory_fd)
    finally:
        if os.path.exists(temporary):
            os.unlink(temporary)


def run(config, dry_run=False, check_only=False, interval=30):
    if interval < 0:
        raise ValueError("interval must be nonnegative")
    if dry_run:
        revision, archive, plan = build_plan(config)
        print("DRY RUN: revision={} archive={} cases={}".format(revision, archive, len(plan)))
        return 0
    state_path = required_path(config, "server.clock_state_file")
    state_path.parent.mkdir(parents=True, exist_ok=True)
    with (state_path.parent / (state_path.name + ".lock")).open("a+") as lock:
        try:
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError:
            raise RuntimeError("another clock invocation is running")
        status = request(config, "query_status", {})
        active = sum(status[key] for key in ("pending", "queued", "assigned", "running"))
        if check_only:
            print("unfinished={} total={}".format(active, status["total"]))
            return 0
        if active:
            print("SKIPPED: {} unfinished tasks".format(active))
            return 0
        previous = json.loads(state_path.read_text(encoding="utf-8")) if state_path.exists() else None
        if previous:
            if previous.get("phase") != "complete" or previous.get("uncertain"):
                raise RuntimeError("previous submission uncertain; inspect " + str(state_path))
            for task_id in previous.get("task_ids", []):
                task = request(config, "query_task", {"task_id": task_id})
                if task["status"] not in TERMINAL:
                    raise RuntimeError("previous task is unfinished: {}".format(task_id))
            if previous.get("day") == date.today().isoformat():
                print("SKIPPED: suite already submitted today")
                return 0
        revision, archive, plan = build_plan(config)
        protect = required_path(config, "server.protect_versions_file")
        atomic_text(protect, revision + "\n")
        state = {"day": date.today().isoformat(), "revision": revision,
                 "archive": str(archive), "phase": "submitting",
                 "task_ids": [], "uncertain": False, "planned": len(plan)}
        atomic_text(state_path, json.dumps(state, sort_keys=True))
        for index, entry in enumerate(plan, 1):
            state["uncertain"] = True
            atomic_text(state_path, json.dumps(state, sort_keys=True))
            answer = request(config, "submit_task", entry)
            task_id = answer["task_id"]
            state["task_ids"].append(task_id)
            state["uncertain"] = False
            atomic_text(state_path, json.dumps(state, sort_keys=True))
            print("[{}/{}] task_id={} template={} case={}".format(
                index, len(plan), task_id, entry["payload"]["flow"], entry["payload"]["case"]),
                flush=True)
            if index < len(plan):
                time.sleep(interval)
        state["phase"] = "complete"
        atomic_text(state_path, json.dumps(state, sort_keys=True))
        print("SUBMITTED: {} tasks at revision {}".format(len(plan), revision))
    return 0


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--dry-run", action="store_true")
    parser.add_argument("--check-only", action="store_true")
    parser.add_argument("--interval", type=float, default=30)
    args = parser.parse_args()
    try:
        return run(load_config(), args.dry_run, args.check_only, args.interval)
    except (OSError, ValueError, RuntimeError, KeyError, TypeError) as error:
        print("[CLOCK] " + str(error), file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
