"""Worker output retention never discards active or unreported results."""
import json
import os
from pathlib import Path
import sys
import tempfile
import time

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "worker"))
from log_retention import prune_worker_logs


def old_tree(root, task_id, old):
    path = root / "run-example" / str(task_id)
    path.mkdir(parents=True, exist_ok=True)
    file = path / "stdout.log"
    file.write_text("case log")
    os.utime(str(file), (old, old))
    os.utime(str(path), (old, old))
    return path


def main():
    with tempfile.TemporaryDirectory(prefix="forge-log-retention-") as temp:
        root = Path(temp)
        now = time.time()
        old = now - 31 * 86400
        expired = old_tree(root, 1, old)
        pending = old_tree(root, 2, old)
        active = old_tree(root, 3, old)
        unknown = old_tree(root, 4, old)
        recent = old_tree(root, 5, now)
        fresh_empty = root / "run-fresh"
        fresh_empty.mkdir()
        os.symlink(str(root / "outside"), str(unknown / "link"))
        (root / "pending-logs-run2.json").write_text(json.dumps({
            "task_id": 2, "directory": str(pending)}))
        (root / "active-slot.json").write_text(json.dumps({"task_id": 3}))
        assert prune_worker_logs(root, 30, now) == 1
        assert not expired.exists()
        assert pending.exists() and active.exists() and unknown.exists() and recent.exists()
        assert fresh_empty.exists()
        # A malformed outbox blocks all pruning rather than losing unreported evidence.
        (root / "pending-report-bad.json").write_text("{}")
        old_tree(root, 6, old)
        try:
            prune_worker_logs(root, 30, now)
        except RuntimeError:
            pass
        else:
            raise AssertionError("malformed outbox was not protected")
        assert (root / "run-example" / "6").exists()
    print("log retention PASS")


if __name__ == "__main__":
    main()
