# Production hardening

## Task hierarchy

One `Tasks.yaml` entry produces one daily parent task (internally identified by
the existing `batch_id`). Even repeated template names create separate parents.
Each run.tcl remains one independently scheduled execution (`task_id` in the
existing wire protocol). The Clock state `batch_id` identifies the invocation;
`group_ids` contains the user-visible daily tasks. Old saved plans resume with
their original IDs. No historical database records are rewritten or deleted.

Cancelling a parent skips its remaining submissions and queued cases; active
cases finish normally. It does not stop later YAML entries. Completed-run checks
use parent summaries, with per-case checks retained for old Clock state files.

The GUI requests grouped rows, not thousands of case rows. Details and exports
load every case in the selected parent using a bounded immutable query snapshot.
Expired/evicted snapshots fail explicitly: partial results are not exported.
Queries are capped at 50 rows/page and 64 MiB total retained snapshot content.
Snapshots expire after two minutes; up to eight coexist. Broad ungrouped queries
must use batch filters if they exceed that budget.

## Persistence and evidence

Only active records are restored into the scheduler hot set. Terminal records
are evicted at submission boundaries when the hot set exceeds 2048 entries;
SQLite retains their results. IDs and FIFO sequence high-water marks come from
the database. Parent lists stream history from SQLite; details use the batch
index, and status counts use SQL aggregation. Regression queries retain only
matching records rather than copying all unrelated history.

This bounds resident history; it does not promise constant-time historical
queries. Large regression/progress queries can still require scans and should
be measured against real retained data before adding materialized indexes.

The durable result outbox includes archive retry intent. Transient manifest-write
failure does not silently lose the upload obligation or acknowledge completion.
Legacy outboxes still replay. Log I/O uses two dedicated threads and a 64-job
queue, outside the global session lock. ACKs still follow durable file sync.
Half-close retains a pending-response lease. Overload fails explicitly for retry.

## Log rotation

`start.sh` now pipes console output to a separate rotating sink. It rotates by
day and size (default 10 MiB), and prunes closed known console logs after 30 days.
Live/open files, unknown files and execution recovery journals are preserved.
Optional role-config override: `log.service_max_bytes=10485760`.

Find current console files with `find logs -name '*-service*.log'` or inspect
`status.sh`. Existing running services must be gracefully restarted to use this
sink. External cron redirection files such as `logs/clock-cron.log` are not owned
by this sink; use site logrotate policy or rely on Clock's `logs/daily` logs.

## Database backups

Before real daily submission, Clock creates one consistent SQLite backup per
day (not on dry-run/check-only). Default: `<database directory>/backups`, seven
days. WAL contents are included; Python 3.6 uses a transactional logical-copy
fallback. Backups are integrity-checked and atomically published with private
permissions. Low space or a corrupt existing daily backup blocks submission
with an error, rather than silently treating an invalid backup as success.

Manual backup: `python3 -B tools/backup_database.py` from the project root.
Optional Server config fields: `database.backup_root`, `database.backup_days`,
`database.min_free_bytes` (default 512 MiB), `database.backup_enabled=false`.
Same-volume backup is not disaster recovery: copy verified backups off-host.
Capacity checks: `python3 -B tools/health_check.py --role server` (or `worker`).
The command is read-only and exits nonzero on low capacity/missing directories;
run it through site monitoring/cron notifications. Defaults: 512 MiB or 10% free.
It never deletes journals, SVN copies or active logs to reclaim space.
Restore only into a separate path while the target Server is stopped; never
overwrite an in-use DB or delete its WAL/journals to make a test pass.

## Deployment and remaining security decision

Update Server, GUI, Worker and Clock together; keep private role configs and
runtime/SVN data. GUI startup remains `gcrp_gui --ip ADDRESS --port PORT`.
No auto migration of old merged daily batches is attempted.

Authentication is deferred by explicit user choice. All remote Worker pause,
resume and SVN-update requests are rejected by the Server before any state
mutation. There is no configuration switch to bypass this restriction. GUI
management buttons are disabled even against older Servers advertising those
operations. Record deletion remains disabled. Ordinary submit/query and queued
task/batch cancellation remain available; this is not user ownership isolation.

Persisted PENDING maintenance commands are not replayed through heartbeats.
Already-running maintenance may report completion; it is not forcibly killed.
Existing paused/quarantined policies remain intact and are not silently reset.
Before deployment, finish outstanding maintenance and ensure intended Workers
are accepting tasks. If a policy is already paused, an administrator must inspect
it locally during a planned maintenance window; do not delete the database or
recovery journals to force an online state. Startup stays IP/port-only and no
credentials are added to the GUI, CLI, Clock or Worker in this release.

Run the complete suite with `(cd build && ctest --output-on-failure)`.
`test_task_groups` covers 5600 cases grouped into eight 700-case tasks, complete
detail pagination, stable snapshots during mutations, and persisted cold history.
`test_log_io_process` covers real network archives, concurrent status queries and
half-close. `test_hardening` injects archive-intent failure and verifies native
and Python-3.6-compatible backup/restore paths. Real intranet load and target
Python/Qt versions still require deployment acceptance.
