"""Fail-closed single-case PJtest execution in a private per-run workspace."""
import fcntl
import hashlib
import json
import os
from pathlib import Path
import re
import shlex
import shutil
import stat
import time
import uuid
import zipfile

from process_executor import ProcessExecutor, ExecutionContainmentError

_SAFE_REVISION = re.compile(r"[A-Za-z0-9][A-Za-z0-9_.-]{0,63}\Z")
_SAFE_FLOW = re.compile(r"[A-Za-z0-9][A-Za-z0-9_-]{0,63}\Z")
_RESULT_KEY = re.compile(r"[A-Z][A-Z0-9_]*\Z")
_MAX_RESULT_BYTES = 65536


class AdapterError(RuntimeError):
    pass


def tree_digest(root):
    """Content identity of the execution inputs, excluding generated runtime."""
    root = Path(root)
    digest = hashlib.sha256()
    def visit(directory):
        for path in sorted(directory.iterdir()):
            relative = path.relative_to(root).as_posix()
            if path.name in ('.svn', '__pycache__') or relative == 'vivado_runner/runtime':
                continue
            if path.is_symlink():
                raise AdapterError("symlink in test2 inputs: " + relative)
            mode = path.stat().st_mode
            if stat.S_ISDIR(mode):
                digest.update(b'D\0' + relative.encode() + b'\0')
                visit(path)
            elif stat.S_ISREG(mode):
                digest.update(b'F\0' + relative.encode() + b'\0')
                digest.update(str(stat.S_IMODE(mode)).encode() + b'\0')
                digest.update(str(path.stat().st_size).encode() + b'\0')
                with path.open('rb') as stream:
                    for block in iter(lambda: stream.read(1024 * 1024), b''):
                        digest.update(block)
            else:
                raise AdapterError("non-regular test2 input: " + relative)
    visit(root)
    return digest.hexdigest()


def file_digest(path):
    digest = hashlib.sha256()
    with Path(path).open('rb') as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b''):
            digest.update(block)
    return digest.hexdigest()


class PJtestAdapter:
    def __init__(self, config_file, worker_id, heartbeat, path_overrides=None):
        config = json.loads(Path(config_file).read_text(encoding="utf-8"))
        if path_overrides:
            config.update(path_overrides)
        self.work_root = Path(config["test2_root"]).expanduser().resolve(strict=True)
        self.artifacts = Path(config["artifact_root"]).expanduser().resolve(strict=True)
        self.log_root = Path(config["log_root"]).expanduser().resolve()
        self.worker_id = worker_id
        self.profiles = config["flow_profiles"]
        self.clean = config.get("clean", True)
        self.environment = config.get("environment", {})
        if (not isinstance(self.environment, dict) or any(
                not isinstance(k, str) or not re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]*", k) or
                not isinstance(v, str) or "\0" in v for k, v in self.environment.items())):
            raise AdapterError("invalid local environment")
        self.grace = float(config.get("terminate_grace_seconds", 2))
        if (not self.work_root.is_dir() or not self.artifacts.is_dir() or
                not isinstance(self.profiles, dict) or not self.profiles or
                type(self.clean) is not bool or not 0 <= self.grace <= 60 or
                not (self.work_root / "run.sh").is_file()):
            raise AdapterError("invalid local PJtest configuration")
        self.log_root.mkdir(parents=True, exist_ok=True)
        self.executor = ProcessExecutor(heartbeat)
        self.healthy = True

    def validate(self, task):
        payload = task.get("payload")
        if (task.get("task_type") != "REGRESSION" or type(task.get("task_id")) is not int or
                task["task_id"] <= 0 or not isinstance(payload, dict) or
                set(payload) != {"spec_version", "case", "flow", "timeout_seconds"} or
                type(payload["spec_version"]) is not int or payload["spec_version"] != 1 or
                type(payload["timeout_seconds"]) is not int or
                not 1 <= payload["timeout_seconds"] <= 86400):
            raise AdapterError("invalid regression payload")
        relative, flow, revision = payload["case"], payload["flow"], task.get("revision")
        if (not isinstance(relative, str) or len(relative) > 512 or
                not re.fullmatch(r"[A-Za-z0-9_.\-/]+", relative) or
                any(x in ("", ".", "..") for x in relative.split("/")) or
                not relative.endswith("/run.tcl") or
                not isinstance(flow, str) or not _SAFE_FLOW.fullmatch(flow) or flow not in self.profiles or
                not isinstance(revision, str) or not _SAFE_REVISION.fullmatch(revision)):
            raise AdapterError("invalid case, flow or revision")
        case = (self.work_root / relative).resolve(strict=True)
        if self.work_root not in case.parents or not case.is_file() or case.name != "run.tcl":
            raise AdapterError("case must be a local run.tcl")
        settings = self.profiles[flow]
        if not isinstance(settings, dict) or not settings:
            raise AdapterError("flow profile must contain local settings")
        for key, value in settings.items():
            if (not isinstance(key, str) or not re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]*", key) or
                    isinstance(value, (dict, list, bool)) or not isinstance(value, (str, int, float)) or
                    "\n" in str(value) or "\r" in str(value)):
                raise AdapterError("invalid local flow setting")
        return case, settings

    def artifact(self, revision):
        candidates = [self.artifacts / name for name in (
            f"GalaxCore_{revision}.zip", f"Galaxcore_{revision}.zip",
            f"GalaxCore_r{revision}.zip", f"GalaxCore-{revision}.zip")]
        found = [p for p in candidates if p.exists() or p.is_symlink()]
        if not found:
            raise AdapterError("build artifact not found for revision " + revision)
        if len(found) != 1 or found[0].is_symlink() or not found[0].is_file():
            raise AdapterError("ambiguous or unsafe artifact")
        return found[0]

    def _prepare(self, artifact, revision, directory):
        manifest_path = Path(str(artifact) + ".manifest.json")
        if manifest_path.is_symlink():
            raise AdapterError("unsafe artifact manifest")
        with manifest_path.open('rb') as stream:
            raw = stream.read(65537)
        if len(raw) > 65536:
            raise AdapterError("invalid artifact manifest")
        manifest = json.loads(raw)
        if (type(manifest.get("spec_version")) is not int or manifest["spec_version"] != 1 or
                manifest.get("revision") != revision or
                not isinstance(manifest.get("test2_revision"), str) or not manifest["test2_revision"] or
                any(not isinstance(manifest.get(k), str) or
                    not re.fullmatch(r"[0-9a-f]{64}", manifest[k])
                    for k in ("archive_sha256", "test2_sha256"))):
            raise AdapterError("invalid artifact manifest")
        # Copy first, validate the private bytes. A producer replacing/changing the
        # source cannot change what will subsequently be installed.
        archive = directory / "artifact.zip"
        if artifact.stat().st_size > 2 * 1024**3:
            raise AdapterError("artifact size limit")
        shutil.copyfile(artifact, archive)
        if file_digest(archive) != manifest["archive_sha256"]:
            raise AdapterError("artifact hash mismatch")
        source = self.work_root
        if tree_digest(source) != manifest["test2_sha256"]:
            raise AdapterError("test2 hash mismatch")
        slot = directory / "workspace"
        slot.mkdir()
        destination = slot / "test2"
        def ignore(parent, names):
            excluded = {n for n in names if n in ('.svn', '__pycache__')}
            if Path(parent) == source / "vivado_runner":
                excluded.add("runtime")
            return excluded
        shutil.copytree(source, destination, symlinks=True, ignore=ignore)
        if tree_digest(destination) != manifest["test2_sha256"]:
            raise AdapterError("test2 changed during snapshot")
        binary = slot / "bin/Linux_64/GalaxCore"
        flow_count = 0
        seen = set()
        with zipfile.ZipFile(archive) as zf:
            items = zf.infolist()
            if len(items) > 100000 or sum(x.file_size for x in items) > 2 * 1024**3:
                raise AdapterError("artifact extraction limit")
            binaries = [x for x in items if not x.is_dir() and
                        Path(x.filename).name in ("GalaxCore", "Galaxcore")]
            if len(binaries) != 1:
                raise AdapterError("artifact must contain one GalaxCore binary")
            for item in items:
                parts = Path(item.filename).parts
                if (item.filename.startswith("/") or "\\" in item.filename or
                        any(x in ("..", ".") for x in item.filename.split("/")) or
                        stat.S_IFMT(item.external_attr >> 16) == stat.S_IFLNK):
                    raise AdapterError("unsafe artifact archive")
                if item.is_dir():
                    continue
                if item == binaries[0]:
                    target = binary
                elif "flow" in parts:
                    if parts.index("flow") == len(parts) - 1:
                        raise AdapterError("invalid flow artifact entry")
                    target = slot / "flow" / Path(*parts[parts.index("flow")+1:])
                    flow_count += 1
                else:
                    continue
                if target in seen:
                    raise AdapterError("duplicate artifact destination")
                seen.add(target)
                target.parent.mkdir(parents=True, exist_ok=True)
                with zf.open(item) as src, target.open("xb") as dst:
                    shutil.copyfileobj(src, dst)
        if not flow_count:
            raise AdapterError("artifact missing flow")
        binary.chmod(0o755)
        (directory / "manifest.json").write_text(json.dumps(manifest, indent=2))
        return destination, binary, manifest

    def _run_clean(self, env, output):
        script = self.work_root / "clean.sh"
        if self.clean and script.is_file():
            _, code, timed_out = self.executor.run(
                ["bash", str(script)], self.work_root, env, output, output, 120, self.grace)
            return 124 if timed_out else code
        return 0

    @staticmethod
    def _write_flow_config(path, settings):
        lines = path.read_text(encoding="utf-8").splitlines() if path.exists() else []
        entry = re.compile(r"^\s*(?:set\s+)?([A-Za-z_][A-Za-z0-9_]*)(?:\s*=\s*|\s+)")
        lines = [line for line in lines if not (entry.match(line) and entry.match(line)[1] in settings)]
        lines.extend(f"{key} {value}" for key, value in sorted(settings.items()))
        path.write_text("\n".join(lines) + "\n", encoding="utf-8")

    @staticmethod
    def _parse(path, case):
        # O_NOFOLLOW plus a strict byte limit, never eval/source shell output.
        fd = os.open(path, os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK)
        with os.fdopen(fd, 'rb') as stream:
            if not stat.S_ISREG(os.fstat(stream.fileno()).st_mode):
                raise AdapterError("invalid_result")
            raw = stream.read(_MAX_RESULT_BYTES + 1)
        if len(raw) > _MAX_RESULT_BYTES:
            raise AdapterError("invalid_result")
        result = {}
        for line in raw.decode("utf-8", "strict").splitlines():
            if not line or line.startswith("#"):
                continue
            if "=" not in line:
                raise AdapterError("invalid_result")
            key, value = line.split("=", 1)
            if not _RESULT_KEY.fullmatch(key) or key in result:
                raise AdapterError("invalid_result")
            # Handles ordinary printf %q escaping (including spaces); reject
            # unsupported ANSI-C quoting rather than interpret arbitrary shell.
            values = shlex.split(value)
            if len(values) != 1:
                raise AdapterError("invalid_result")
            result[key] = values[0]
        if result.get("RUN_TCL") != str(case) or result.get("STATUS") not in (
                "PASS", "FAIL", "FAILED", "ERROR", "TIMEOUT", "INTERRUPTED"):
            raise AdapterError("invalid_result")
        if "RET_CODE" in result:
            try:
                int(result["RET_CODE"])
            except ValueError as error:
                raise AdapterError("invalid_result") from error
        return result

    def execute(self, task, directory):
        if not self.healthy:
            raise AdapterError("worker quarantined after cleanup failure")
        directory = Path(directory).resolve()
        source = self.work_root
        if directory == source or source in directory.parents:
            raise AdapterError("output must not be inside test2 inputs")
        directory.mkdir()
        result = dict(task_id=task["task_id"], worker_id=self.worker_id,
                      revision=task.get("revision"), payload=task.get("payload"),
                      status="FAILED", reason="worker_exception", raw_exit_code=None,
                      exit_code=None, pid=None, started_at=time.time())
        # Kernel lock on the canonical source directory: identity is independent
        # of worker names or configurable lock-root aliases. Source is read-only.
        lock_fd = os.open(source, os.O_RDONLY | os.O_DIRECTORY)
        env = None
        prepared = False
        with (directory / "stdout.log").open("xb") as output, (directory / "stderr.log").open("xb") as err:
            try:
                fcntl.flock(lock_fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
                _, settings = self.validate(task)
                artifact = self.artifact(task["revision"])
                root, binary, manifest = self._prepare(artifact, task["revision"], directory)
                self.work_root = root
                case, _ = self.validate(task)
                namespace = "forge-" + uuid.uuid4().hex
                env = os.environ.copy()
                env.update(self.environment)
                # Do not allow inherited PJTest identities to select another run.
                for key in list(env):
                    if key.startswith(("PJTEST_", "DTS_")) or key in (
                            "GALAXCORE_WORKSPACE_ROOT", "GALAXCORE_BIN", "GALAXCORE_FLOW_CONFIG",
                            "GALAXCORE_WORKER_NAME", "GALAXCORE_TASK_ID", "GALAXCORE_EXAMPLE_ID",
                            "GALAXCORE_ATTEMPT_ID", "GALAXCORE_BUILD_INFO",
                            "RUN_SH_LOCK_FILE", "RUN_SH_LOCK_HELD", "RUN_SH_LOCK_DIR", "VIVADO_RUNNER_NAMESPACE"):
                        env.pop(key)
                env.update({"PJTEST_TASK_ID": str(task["task_id"]), "PJTEST_REVISION": task["revision"],
                            "PJTEST_WORKER_NAME": self.worker_id, "GALAXCORE_RUN_MODE": "distributed",
                            "DTS_RUN_MODE": "distributed", "GALAXCORE_BUILD_REVISION": task["revision"],
                            "GALAXCORE_REVISION": task["revision"], "GALAXCORE_BUILD_ZIP": str(directory / "artifact.zip"),
                            "GALAXCORE_BIN": str(binary), "GALAXCORE_WORKSPACE_ROOT": str(root),
                            "VIVADO_RUNNER_NAMESPACE": namespace,
                            "RUN_SH_LOCK_DIR": str(directory / "locks")})
                config_path = root / "flow_config"
                self._write_flow_config(config_path, settings)
                shutil.copyfile(config_path, directory / "flow_config")
                result.update(artifact=str(artifact), manifest=manifest, case=str(case),
                              workspace=str(root), namespace=namespace)
                prepared = True
                if self._run_clean(env, output) != 0:
                    raise AdapterError("pre_clean_failed")
                selected = root / "vivado_runner/runtime/workspaces" / namespace / "status" / case.parent.relative_to(root) / "result.env"
                if selected.exists():
                    raise AdapterError("result existed before execution")
                argv = ["bash", str(root / "run.sh"), str(case), "--timeout",
                        str(task["payload"]["timeout_seconds"]), "--bg", "1",
                        "--galaxcore", str(binary), "--flow-config", str(config_path)]
                result["argv"] = argv
                def record_start(pid):
                    result["pid"] = pid
                    (directory / "pid").write_text(str(pid) + "\n")
                    (directory / "execution.json").write_text(json.dumps(result, indent=2))
                pid, code, timed_out = self.executor.run(
                    argv, root, env, output, err,
                    task["payload"]["timeout_seconds"] + self.grace + 5, self.grace, record_start)
                result.update(pid=pid, raw_exit_code=code, exit_code=code)
                if timed_out:
                    result.update(status="TIMEOUT", reason="timeout", exit_code=124)
                elif code == 125:
                    result["reason"] = "execution_tree_incomplete"
                elif not selected.is_file():
                    result["reason"] = "missing_result" if code == 0 else "process_failed"
                else:
                    # Only the fresh, randomly named run namespace is eligible.
                    if any(p.is_symlink() for p in (selected, *selected.parents)):
                        raise AdapterError("unsafe result path")
                    data = self._parse(selected, case)
                    shutil.copyfile(selected, directory / "result.env")
                    result.update(result_env=str(selected), case_status=data["STATUS"],
                                  case_reason=data.get("REASON"), case_ret_code=data.get("RET_CODE"))
                    if data["STATUS"] == "PASS":
                        result.update(status="SUCCEEDED", reason="pass", exit_code=0)
                    elif data["STATUS"] == "TIMEOUT":
                        result.update(status="TIMEOUT", reason="timeout", exit_code=124)
                    else:
                        result["reason"] = "business_failed"
            except BlockingIOError:
                result["reason"] = "slot_busy"
            except AdapterError as error:
                result.update(status="FAILED", reason=str(error)[:160])
            except ExecutionContainmentError as error:
                self.healthy = False
                result.update(status="FAILED", reason=str(error), worker_healthy=False)
            except (OSError, ValueError, zipfile.BadZipFile) as error:
                result.update(status="FAILED", reason="worker_exception:" + type(error).__name__)
            finally:
                self.executor.stop(self.grace)
                try:
                    if prepared and self.executor.safe:
                        # Retain bounded diagnostic evidence before destructive cleanup.
                        evidence = directory / "evidence"
                        evidence.mkdir()
                        case_run = self.work_root / task["payload"]["case"]
                        for name, path in (("run", case_run.parent / "run"),):
                            if path.is_file() and not path.is_symlink():
                                with path.open("rb") as src, (evidence / name).open("xb") as dst:
                                    dst.write(src.read(8 * 1024 * 1024))
                        try:
                            clean_code = self._run_clean(env, output)
                        except Exception as error:
                            clean_code = -1
                            result["cleanup_error"] = str(error)[:160]
                        result["cleanup_exit_code"] = clean_code
                        if clean_code:
                            self.healthy = False
                            result.update(status="FAILED", reason="post_clean_failed", worker_healthy=False)
                finally:
                    self.work_root = source
                    os.close(lock_fd)
                    result["finished_at"] = time.time()
                    (directory / "result.json").write_text(json.dumps(result, indent=2) + "\n")
        return result
