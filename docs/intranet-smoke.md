# Two-host intranet smoke test

This test runs **one Server, one Worker machine, one slot, one real PJTest
`run.tcl` twice**. It does not use `--demo`, start a shell command from a Task,
delete a slot checkout, or cancel a running case. It is intentionally smaller
than a multi-slot acceptance test. Both machines need the same ForgeSched code
and their own `config/forgesched.conf`; Python 3.6.8 is supported.

Configure the Server's `config/forgesched.conf`:

```ini
server.bind_ip=<Server LAN IP>
network.server_ip=<Server LAN IP>
server.port=18080
server.work_root=<complete GalaxCore checkout>/test2
server.artifact_root=<directory containing GalaxCore_18925.zip>
server.templates_dir=./server/templates
smoke.revision=18925
smoke.case=kintexuplus/xcku3p-ffvd900-1-i/convergentRoundingEven/run.tcl
smoke.flow=route
smoke.target=xcku3p-ffvd900-1-i
smoke.run_count=2
smoke.case_timeout_seconds=5400
smoke.expected_worker_id=
smoke.report_dir=./intranet-report
```

The flow configuration comes from the Server template
`server/templates/route.json`; change it there only. The Server verifies that
the case and ZIP exist before submitting anything. Use a free port and a
dedicated `database.path` for this run. The script refuses to submit if the
Server already has unfinished Tasks.

Configure the Worker machine's `config/forgesched.conf`:

```ini
network.server_ip=<same Server LAN IP>
server.port=18080
worker.slots=1
worker.slots_root=./worker/workers_slots
worker.output_root=./worker-output
worker.svn_url=<GalaxCore SVN URL>
worker.artifact_root=<directory containing GalaxCore_18925.zip>
worker.checkout_parallelism=1
smoke.revision=18925
smoke.run_count=2
smoke.report_dir=./intranet-report
```

The Worker checks out a **complete** GalaxCore SVN copy automatically when the
slot is missing. An existing partial checkout is not deleted or repaired by
this test. `worker.svn_revision` is the checkout revision; it need not match
the ZIP build revision in `smoke.revision`.

From the ForgeSched root, start these two commands in separate terminals,
Server first and Worker second:

```bash
python3 -B -u tools/intranet_server_smoke.py
python3 -B -u tools/intranet_worker_smoke.py
```

They start the Server and slot manager themselves. After both cases finish,
the Worker drains first; the Server waits for the slot to go offline and then
stops. Each command exits 0 only when its own checks pass. The two JSON files
to copy back are:

```text
<Server ForgeSched>/intranet-report/server-report.json
<Worker ForgeSched>/intranet-report/worker-report.json
```

They contain configuration identifiers, Task IDs, status transitions, case
verdicts, key errors and only a short console tail on failure. Full local
`server-console.log` and `worker-console.log` remain beside the reports if a
later investigation needs them. The report directory is Git-ignored. On a
failure with an unfinished case, the Server may deliberately remain running;
check `server_left_running` in its report before attempting another run.
If the Server never starts, stop the waiting Worker launcher with Ctrl+C;
it cannot distinguish that from a slow initial Server startup on another host.
