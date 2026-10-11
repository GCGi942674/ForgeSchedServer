# P12 Worker loss and restart recovery

## Contract

TCP loss is not execution failure. A live Python Worker continues the current
case while reconnecting; completed results remain in the durable pending outbox
until acknowledged. Registration with `recovery_version=1` holds the Worker
out of scheduling until `worker_recover` reconciles its single slot. Legacy
Workers retain their existing protocol but cannot perform automatic recovery.
The registration hold is separate from the recovered-task hold: replaying a
completed result cannot release the registration hold or trigger a new assignment
before the Worker finishes its recovery handshake.

The recovery request contains `worker_id`, `state` (`idle`, `active`, `lost`),
and, for active/lost, `task_id` and `run_id`. Server state is authoritative.
An idle Worker can abandon an ASSIGNED attempt because TASK_START permission
was never committed. A RUNNING attempt cannot be abandoned merely on timeout.
An active matching attempt resumes; mismatched identities are rejected.

Before TASK_START the Worker fsyncs an active journal. Before each child process
launch (including PJtest cleanup) it fsyncs a unique receipt path in that journal.
The Linux subreaper writes the receipt only after ECHILD proves every descendant
has exited. On Worker restart, a journal with that receipt can send `lost`.
A journal still in the pre-launch phase is also safe. Missing/invalid receipts
quarantine the Worker for inspection; deleting its journal is not a recovery step.
Worker identity is protected by an OS lock in the persistent output directory.
Use one stable output directory and globally unique Worker ID per slot.

Recovery of a proven-lost run atomically records a recovery decision and closes
the old attempt. `server.recovery_max_retries` (default 1, range 0..100) bounds
automatic retries of RUNNING attempts; business FAIL/TIMEOUT are not retried by
this policy. A stopped batch cancels the would-be retry. A fresh assignment has
a new run_id. Duplicate recovery requests return the saved decision and cannot
increment retry_count twice, including after a Server restart.

P11 schema V1 migrates transactionally to V2 by adding recovery_decisions.
Keep the database on local disk. Server and Worker should be upgraded together.
Permanent host loss without proof of execution exit remains quarantined; this
release does not claim distributed exactly-once execution or remotely kill cases.

## Deployment and operation

Shared configuration (all paths retain the P11 configuration rules):

```ini
server.worker_timeout_seconds=30
server.recovery_max_retries=1
worker.reconnect_seconds=2
```

The Server checks liveness once per second. An expired heartbeat or a closed
connection marks the Worker offline without freeing its occupied slot. QUERY_TASK
then shows `recovery_required=true`, `recovery_reason=worker_offline` while the
task is in flight. A valid heartbeat on the current connection restores liveness.
Timeout uses the existing Server heartbeat clock; clock jumps can temporarily
affect liveness detection but never authorize a retry.

Retain `worker.output_dir` across Worker restarts: it contains the identity lock,
active journal, pending report and cleanup receipts. Never share a Worker ID
between machines or select a new output directory as a workaround for quarantine.
Receipt files are retained as execution evidence. Before upgrading, stop the
Server and back up its database with the WAL state preserved. V2 cannot be opened
by the P11 binary; downgrading requires restoring the corresponding V1 backup.

Intranet acceptance should first repeat the known passing single GalaxCore case.
Then restart only the Server during execution: the final run_id must be unchanged
and retry_count must remain 0. For Worker crash injection, use a disposable demo
case first; restart with the same identity/output after the cleanup receipt appears.
The retried task must get a new run_id and retry_count 1. Do not use a production
case to test missing cleanup evidence: that state intentionally requires inspection.

This protocol trusts the registered Worker to report its local exit evidence;
it is not an authentication mechanism. Receipts prove process containment, not
business success. If a process completed but its result was never persisted, the
attempt is treated as lost and may execute again within the configured retry bound.

## Acceptance

Tests must exercise a real Server and Worker across Server SIGKILL/restart,
Worker SIGKILL/restart, durable result replay, old-run rejection, bounded retry,
stopped-batch recovery, missing cleanup proof and duplicate recovery messages.
The existing cancellation semantics remain: stop queued cases; let active cases
finish. Offline fixtures do not replace an intranet GalaxCore acceptance run.
