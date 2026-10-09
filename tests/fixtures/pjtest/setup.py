#!/usr/bin/env python3
"""Create a disposable one-slot PJtest fixture for the C++ network test."""
import json
from pathlib import Path
import sys
import zipfile
sys.path.insert(0, str(Path(__file__).resolve().parents[3] / "worker"))
from pjtest_adapter import tree_digest, file_digest

root = Path(sys.argv[1])
test2 = root / "slot" / "test2"
artifacts = root / "artifacts"
logs = root / "worker-logs"
locks = root / "locks"
for path in (test2, artifacts, logs, locks):
    path.mkdir(parents=True, exist_ok=True)
modes = ("success", "business_fail", "process_fail", "timeout", "missing",
         "invalid", "stale", "wrong_case", "pass_nonzero", "missing_artifact", "cleanup_fail")
for mode in modes:
    case = test2 / "cases" / mode
    case.mkdir(parents=True)
    (case / "run.tcl").write_text("# fake\n")
script = r'''#!/bin/bash
grep -q '^route_design 1$' flow_config || exit 9
grep -q '^original 1$' flow_config || exit 9
[ "$GALAXCORE_BUILD_REVISION" = 58231 ] || exit 9
[ -f "$GALAXCORE_BUILD_ZIP" ] || exit 9
[ -x "$GALAXCORE_BIN" ] || exit 9
[ "$GALAXCORE_WORKSPACE_ROOT" = "$PWD" ] || exit 9
case_dir=$(dirname "$1")
mode=$(basename "$case_dir")
status_dir="vivado_runner/runtime/workspaces/$VIVADO_RUNNER_NAMESPACE/status/cases/$mode"
mkdir -p "$status_dir"
if [ "$mode" = timeout ]; then sleep 2; exit 124; fi
if [ "$mode" = process_fail ]; then exit 7; fi
if [ "$mode" = stale ] || [ "$mode" = missing ]; then exit 0; fi
if [ "$mode" = invalid ]; then printf 'STATUS=PASS\n' > "$status_dir/result.env"; exit 0; fi
if [ "$mode" = wrong_case ]; then
  printf 'RUN_TCL=/different/run.tcl\nSTATUS=PASS\n' > "$status_dir/result.env"
  exit 0
fi
if [ "$mode" = business_fail ]; then status=FAIL; else status=PASS; fi
printf 'RUN_TCL=%s\nSTATUS=%s\nRET_CODE=7\n' "$1" "$status" > "$status_dir/result.env"
if [ "$mode" = cleanup_fail ]; then touch .fixture_fail_clean; fi
if [ "$mode" = pass_nonzero ]; then exit 7; fi
exit 0
'''
(test2 / "run.sh").write_text(script)
(test2 / "flow_config").write_text("original 1\n")
(test2 / "clean.sh").write_text("#!/bin/bash\n[ ! -e .fixture_fail_clean ] || exit 9\nprintf 'clean\\n' >> .fixture_clean_calls\nexit 0\n")
stale = test2 / "vivado_runner/runtime/workspaces/fixture/status/cases/stale/result.env"
stale.parent.mkdir(parents=True)
stale.write_text(f"RUN_TCL={test2 / 'cases/stale/run.tcl'}\nSTATUS=PASS\n")
with zipfile.ZipFile(artifacts / "GalaxCore_58231.zip", "w") as zf:
    zf.writestr("bin/Linux_64/GalaxCore", "#!/bin/sh\nexit 0\n")
    zf.writestr("flow/marker.txt", "fake\n")
(artifacts / "GalaxCore_58231.zip.manifest.json").write_text(json.dumps({
    "spec_version": 1, "revision": "58231", "test2_revision": "fixture-1",
    "archive_sha256": file_digest(artifacts / "GalaxCore_58231.zip"),
    "test2_sha256": tree_digest(test2)
}))
(root / "worker.json").write_text(json.dumps({
    "test2_root": str(test2), "artifact_root": str(artifacts),
    "log_root": str(logs), "lock_root": str(locks),
    "flow_profiles": {"route": {"route_design": 1}},
    "clean": True, "terminate_grace_seconds": 0.1
}))
print("{}")
