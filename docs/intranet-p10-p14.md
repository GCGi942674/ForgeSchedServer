# P10–P14 three-host intranet acceptance

This one Python 3.6-compatible script is run once on pudong (controller) and
once each on novarch3 and novarch4 (Worker agents). It performs queued batch
cancellation, SQLite restart, Server-crash recovery during a real case,
3-to-5 slot growth, and Regression history/pagination. It also requires the
existing P10–P14 offline fixtures to pass on pudong, including disposable
demo Worker-crash recovery. It does **not** crash a real GalaxCore Worker or
claim five simultaneous Vivado cases have been tested.

## Before starting

- All three hosts need the same GCRP source, especially this script.
  The controller verifies the script hashes. Start from each GCRP root.
- Stop all other Server processes on the configured `server.port`, and other
  slot managers on both Worker hosts. The script refuses an occupied Server
  port or an already-owned manager lock. It creates a unique Server SQLite
  database under `smoke.report_dir/p10-p14-<run-id>/state.db`; the configured
  production database is never opened by its Server child.
- Keep the known passing `smoke.revision`, `smoke.case`, `smoke.flow`, ZIP and
  route template. The case must last long enough for P12 to catch it RUNNING
  before the Server restart. Do not use a production-only case that cannot be
  safely rerun in a disposable acceptance batch.
- On both Workers, keep their distinct auto-generated identities and their
  normal `worker.output_root`, `worker.slots_root`, SVN URL and artifact root.
  **Do not copy worker-output between machines.** The Worker agents create
  temporary config copies with `worker.slots=1`, then grow novarch4 to 3 and
  5; the real config file is not edited.
- novarch4 needs room for as many as **four additional complete GalaxCore SVN
  copies** if slot2–5 do not exist. Check disk capacity before starting. The
  script neither removes partial checkouts nor cleans old reports.
- TCP `server.port` and `smoke.control_port` must be reachable from both
  Workers. The temporary control port defaults to `server.port+1` (for port
  8080, TCP 8081). Optionally set `smoke.control_port` to the same free value
  in all three configs. This is a short-lived, token-protected test channel;
  it cannot execute arbitrary shell commands.
- Build the complete test suite on pudong first (not only `server`), and
  ensure SQLite development headers are installed. Worker hosts run Python
  and SVN; they do not need to compile C++ for this script.

## Start in three terminals

On pudong:

```bash
python3 -B -u tools/intranet_p10_p14.py --role server --scale-host novarch4
```

The controller prints a one-time `--token` command. Copy **that command** to
each Worker host, from its GCRP root, for example:

```bash
python3 -B -u tools/intranet_p10_p14.py --role worker --token TOKEN_FROM_PUDONG
```

This invocation works in both bash and csh; do not use Bash-only environment
assignment syntax. The controller waits for both agents, then starts its own
Server and progresses automatically. The token is not saved in the reports.
Do not separately launch `build/bin/gcrp_server` or `worker/slot_manager.py`.

The Server uses one Worker slot for P10–P12. P10 submits three real cases in
one batch, waits for one RUNNING and two QUEUED, cancels the batch, then waits
for the active case to finish. P11 gracefully restarts only the Server and
checks the same database. P12 SIGKILLs **only the Server process started by
this script** while another real case is RUNNING; the Worker keeps executing
and reconnects. P13 starts the second host and grows novarch4 1→3→5, checking
all six registrations and SVN checkouts. P14 queries the two test batches one
result per page and verifies PASS/CANCELLED counts. Finally the controller
runs the local P10–P14 fixtures, asks Workers to drain, and stops its Server.

## Results and failure handling

The Server writes `intranet-report/p10-p14-<run-id>/server-report.json` and
each Worker writes `intranet-report/p10-p14-<run-id>/worker-report.json`.
Send all three reports to Codex; on failure, also send the corresponding
`server-console.log`, `manager-console.log`, and any named
`stage-test-logs/<test>.log` from that run directory. The report has a
per-stage `PASS` only after its assertions pass; a global `PASS` also requires
all nine offline fixtures.

Failure leaves active real tasks, their Server database, Worker journals and
SVN directories for inspection. **Do not delete them, and do not start a
second run on the same port until checking the recorded Server/manager PIDs.**
Use Server `task_id`/`run_id` for cross-host correlation; the script never
compares wall-clock timestamps from different machines.
