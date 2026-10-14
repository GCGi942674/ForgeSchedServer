# Two-machine, one-slot-per-machine intranet acceptance

This follows the passing single-machine/two-slot test. It tests **one Server
and two separate Worker machines, each with one slot**. Two real PJTest cases
must finish as PASS on distinct Worker and machine IDs, with overlapping
Server-recorded execution intervals and no retries. It does not deliberately
disconnect or kill a running GalaxCore/Vivado process.

Use the same GCRP commit on all three hosts, one shared ZIP revision
accessible on each machine, and the same Server IP/port. Keep the previously
successful Server work root, artifact root, route template, and case path.
Use a free port and a dedicated `database.path`. Do not run another smoke
script, slot manager, or Server on the same port during this test.

Server `config/gcrp.conf` example:

```ini
smoke.revision=18954
smoke.case=kintexuplus/xcku3p-ffvd900-1-i/convergentRoundingEven/run.tcl
smoke.flow=route
smoke.target=xcku3p-ffvd900-1-i
smoke.run_count=2
smoke.expected_worker_ids=
smoke.require_distinct_machines=1
smoke.report_dir=./intranet-report
database.path=./data/gcrp-two-machine-smoke.db
```

Each slot registers itself. The acceptance script discovers two ONLINE slots;
there is no Server-side Worker ID list to maintain. Give the second physical
host a different hostname. The Server checks that the
registered machine IDs differ. Use separate physical hosts as instructed;
software identity alone cannot prove where processes run. This two-task test
requires exactly two ONLINE slots; the optional `smoke.expected_worker_ids`
can pin their full IDs when diagnosing a mixed deployment.
`smoke.expected_worker_id` (singular) is not read in this mode.

On novarch4, configure exactly one local slot and retain its existing SVN
checkout:

```ini
worker.slots=1
smoke.revision=18954
```

On the second machine, configure one slot:

```ini
worker.slots=1
smoke.revision=18954
```

Both Worker hosts also need `network.server_ip`, `server.port`,
`worker.svn_url`, `worker.artifact_root`, `worker.slots_root`, and
`worker.output_root` set as in the successful one-slot test. Each Worker
launcher checks for its own ZIP and SVN checkout. Missing slots are checked
out automatically; existing checkouts are not deleted or renamed.
`smoke.run_count=2` is a **Server** setting for the total number of Tasks;
each Worker launcher expects only its own one result.

Start these in order, each from its GCRP root:

```bash
# Server machine
python3 -B -u tools/intranet_dual_server_smoke.py
# Worker machine A
python3 -B -u tools/intranet_two_machine_worker_smoke.py
# Worker machine B
python3 -B -u tools/intranet_two_machine_worker_smoke.py
```

The Server reuses the dual-task launcher with
`smoke.require_distinct_machines=1`. Return the Server's
`intranet-report/dual-server-report.json` and both Workers'
`intranet-report/two-machine-worker-report.json`. The Server report must
show different `machine_ids`, different `worker_id` values, positive
`overlap_ms`, two clean PASS verdicts and `workers_drained=true`.
Each Worker report must show one PASS result and `checkout_complete=true`.
If a script fails, send its local `*-console.log` and report. If the Server
report has `server_left_running=true`, inspect the real process state before
restarting or cleaning anything.

After this normal-path run, plan Worker-disconnect and recovery tests
separately. Do not stop a live real testcase just to simulate a fault.
