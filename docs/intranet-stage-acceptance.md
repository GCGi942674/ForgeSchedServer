# P9–P14 intranet acceptance checklist

Use the same ForgeSched revision on the Server and Worker hosts. Keep the
known-good case, flow template and ZIP revision. Give smoke runs a dedicated
Server database and port; do not run two Server launchers at once. These tests
may start real GalaxCore cases and can take as long as their configured case
timeouts. The stage audit below is read-only except for writing its report and
running local disposable fixture tests.

## 1. Normal real-workload path (P9 and P13)

Run one of these pairs, **Server command first**, Worker command second:

| Scenario | Server host | Worker host(s) |
| --- | --- | --- |
| One host, one slot | `python3 -B -u tools/intranet_server_smoke.py` | `python3 -B -u tools/intranet_worker_smoke.py` |
| One host, two slots | `python3 -B -u tools/intranet_dual_server_smoke.py` | `python3 -B -u tools/intranet_dual_worker_smoke.py` |
| Two physical Worker hosts | `python3 -B -u tools/intranet_dual_server_smoke.py` | On **each** host: `python3 -B -u tools/intranet_two_machine_worker_smoke.py` |

Configure `worker.slots=1` or `2` as appropriate. For the two-host run,
configure `smoke.require_distinct_machines=1` on the Server. Follow
[single-slot](intranet-smoke.md), [dual-slot](intranet-dual-smoke.md), and
[two-machine](intranet-two-machine-smoke.md) setup. The launchers produce
compact `*-report.json` files in `smoke.report_dir` and exit after their
cases finish. Do not kill a real Vivado case for a smoke test.

## 2. Collect phase evidence

On the Server, after building the complete C++ test suite:

```bash
python3 -B -u tools/intranet_stage_audit.py --side server --run-offline-tests
```

On each Worker host:

```bash
python3 -B -u tools/intranet_stage_audit.py --side worker
```

The Server writes `stage-server-report.json`; each Worker writes
`stage-worker-report.json`. Copy those files and the relevant smoke reports
back for review. If a local test fails, also copy only its named
`stage-test-logs/<test>.log`. This audit does **not** start, stop or signal a
real Server, Worker, slot or case. Running it while the Server is up adds
read-only P14 query evidence. If the Server is stopped, its live query is
marked unavailable, not passed. The P11 check reads the configured SQLite
database in read-only mode and compares IDs from the previous single-slot
smoke; it is **not** a restart test. Stale smoke reports should be removed or
archived only after the corresponding run is known complete; do not treat
their presence as proof of this deployment.

The offline tests cover the implementation, not the intranet environment:

- P10: queued/batch cancellation fixture.
- P11: SQLite and real Server/Worker persistence fixtures.
- P12: recovery state machine and disposable process-fault fixtures.
- P13: scheduling distribution, multi-slot lifecycle and identity fixtures.
- P14: regression-query outcome/pagination fixture.

`EVIDENCE_COLLECTED` means the audit completed; it is **not** a P9–P14 PASS.
Missing, failed and unverified checks remain visible in its JSON.
If a selected local test is missing or fails, the Server audit exits nonzero
and records `OFFLINE_CHECKS_FAILED`; finish building before rerunning. It
never treats a missing test executable as a passing test.

## 3. Real-environment checks not automated here

Perform these only in an isolated test deployment after all active cases are
finished, unless the specific check intentionally observes a disposable case:

1. P10: Submit a batch with more cases than available slots, stop that batch,
   verify queued cases become CANCELLED and the already-running case finishes
   and releases its slot. Never assume a running case was remotely killed.
2. P11: With a dedicated DB, complete a case, stop the Server cleanly, restart
   it with the **same** DB (including WAL state), then query the same task ID.
   Confirm status/result persist and a new task gets a distinct ID. Do not
   move or delete database files while the Server is running.
3. P12: In a disposable deployment, restart only the Server during a case and
   verify the run ID/retry count remain correct. Worker-crash recovery requires
   confirmed child exit and cleanup receipt; never simulate this by killing a
   production GalaxCore/Vivado process or deleting recovery journals.
4. P13: Set `worker.slots=3`, inspect three ONLINE registrations and complete
   a case. Change to `5`, send `SIGHUP` to the manager or cleanly restart it,
   and inspect five ONLINE registrations. Confirm slots 1–3 kept their SVN
   copies and slots 4–5 have complete new copies. Shrink only after cases
   finish. The Worker audit records the expected names and checkout state.
5. P14: With the real Server running, query the smoke revision/case through
   `forgesched_client regression`; compare outcomes and task IDs with the
   Server smoke report. Check pagination when there are more than 50 matching
   tasks. The audit performs the first-page comparison when possible.

P10–P12 fault tests and P13 scaling are not marked verified by a normal
single/dual-slot PASS. If a run is unfinished, preserve the Server database,
Worker output journal and slot directory for diagnosis. Do not delete them to
make a rerun pass.
