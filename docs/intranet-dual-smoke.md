# Dual-slot intranet acceptance

This is the follow-up to the passing one-slot smoke test. It runs one Server,
one Worker machine with **two independent SVN slot checkouts**, and submits the
same real `run.tcl` twice. The run passes only if the two Tasks finish with
PJTest PASS on different Worker IDs, their execution intervals overlap, neither
retries, and both slots drain. It does not test two Worker machines, a
disconnect, or server restart.

Use a dedicated Server database and a free TCP port. Do not run the original
one-slot smoke scripts or another slot manager at the same time. Keep both
machines on the same ForgeSched commit. Leave the existing slot checkout
intact; the second slot checks out its own complete GalaxCore SVN copy.

On the Server, keep the previously successful `server.work_root`,
`server.artifact_root`, `server.templates_dir`, IP, port and case. Set:

```ini
database.path=./data/forgesched-dual-smoke.db
smoke.revision=18954
smoke.case=kintexuplus/xcku3p-ffvd900-1-i/convergentRoundingEven/run.tcl
smoke.flow=route
smoke.target=xcku3p-ffvd900-1-i
smoke.run_count=2
smoke.case_timeout_seconds=5400
smoke.expected_worker_ids=
smoke.report_dir=./intranet-report
```

The Worker manager registers each slot automatically. The test discovers the
two ONLINE slots from Server status; no Worker ID list needs to be maintained
on the Server. This two-task test requires exactly two ONLINE slots to keep
the distribution check unambiguous. `smoke.expected_worker_ids` can optionally
pin the two full IDs when diagnosing a mixed deployment.
`smoke.expected_worker_id` from the one-slot test is not used.

On the Worker machine, keep its successful IP, port, artifact path, SVN URL,
output root and slots root. Set two local slots:

```ini
worker.slots=2
worker.checkout_parallelism=1
smoke.revision=18954
smoke.run_count=2
smoke.report_dir=./intranet-report
```

`worker.checkout_parallelism=1` limits simultaneous SVN checkout, **not**
task execution. Both slots run independently once ready. The ZIP revision
and route flow template must be the same on both hosts. Before starting,
confirm the Worker host has enough disk space for another complete checkout
and enough CPU/memory for two concurrent real cases.

From each ForgeSched root, start Server first, then Worker:

```bash
python3 -B -u tools/intranet_dual_server_smoke.py
python3 -B -u tools/intranet_dual_worker_smoke.py
```

The scripts launch Server and slot manager themselves; do not separately run
those processes. The important files to return are
`intranet-report/dual-server-report.json` and
`intranet-report/dual-worker-report.json`. A PASS report includes two
different `worker_id` values and positive `overlap_ms`. The scripts also
write local `dual-server-console.log` and `dual-worker-console.log`; send
those only if a report fails. If an active task fails or times out, the Server
may be left running for inspection; check `server_left_running` and do not
delete the slot or database while a real case may still be active.
