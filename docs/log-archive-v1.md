# Run log archive v1

Server stores evidence under `server.log_archive_root/TASK_ID/RUN_ID/`. Keep this
directory on durable storage. The default is `./data/run-logs`; it is separate
from the Server diagnostic `log.dir` and the Worker `worker.output_dir`.

After a terminal TASK_RESULT, the Python Worker writes an fsynced local outbox
entry, uploads files in 16 KiB hex-encoded chunks, and removes the outbox only
after Server acknowledgement. Disconnects and Server restarts resume at the
Server's durable offset. An idle Worker retries incomplete uploads. Upload
failure is visible in Worker output and does not change task PASS/FAIL/TIMEOUT.
The archive accepts both the current and persisted older finished run_id values
for a task, but only from that run's Worker connection.

The first supported evidence set is `stdout.log`, `stderr.log`, `result.env`
(when present), `result.json`, `flow_config` (when present), and the bounded
`evidence/run` copy (`evidence.run` in the archive). It also captures the
namespace-isolated vivado_runner `logs/run.log` (`runner.log`) and the single
case `status/.../run.log` (`case-run.log`) before slot cleanup. This captures
run.sh output and core runner/case logs on success and failure. Other tool-
specific stage files and full-text search are not yet included. Each file is
limited to 1 GiB; skipped oversized files are reported by the Worker.
A content SHA-256 manifest and per-stage artifact inventory are not part of
this first version; do not treat this archive as a forensic integrity proof.

To inspect a completed task, get its `run_id` from `query TASK_ID`, then:

```sh
./build/bin/gcrp_client logs TASK_ID --run-id RUN_ID
./build/bin/gcrp_client log TASK_ID --run-id RUN_ID --name stdout.log --offset 0 --limit 8192
```

`log` returns `data_hex` in a JSON response; decode hex for display. Use
`next_offset` until it reaches `size`. An item is complete only when its
`complete` flag is true. Client reads are restricted to a finished run owned
by the requested task. Worker writes require its bound connection identity.
These role checks are **not authentication**: use the existing trusted-LAN
deployment boundary; do not expose this port to untrusted clients.

No schema migration is needed: run ownership is looked up from existing
`task_runs`; archive bytes live in the configured directory. Back up that
directory together with SQLite if log retention matters. Deleting a task is
not implemented in this version.

## 30-day log retention

`log.retention_days=30` is shared by Server and Worker (default 30 when absent).
The persistent Server checks at startup (within one second) and hourly:
recognized files under `server.log_archive_root/TASK_ID/RUN_ID`, using the
newest file's modification time, and `server.log`, `task.log`, `error.log`
under dated `log.dir/YYYYMMDD` folders. Unknown files, symlinks and unexpected
directory names are left untouched. The Worker checks while idle, then hourly,
and removes only old per-task `worker.output_dir/run-*/TASK_ID` execution
directories. Active journals, pending results and pending log-upload outboxes
protect their task output. This policy does **not** remove SQLite task/history
records, SVN slot checkouts, or test/acceptance reports under `intranet-report`.
It also does not erase tool-specific files inside the SVN checkout; those are
managed by the runner's existing cleanup. Backups follow their own retention
policy. File age is based on filesystem timestamps; late uploads are retained
from their upload time.
