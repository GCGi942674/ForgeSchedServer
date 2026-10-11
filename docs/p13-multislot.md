# P13 multiple Workers and isolated slots

P12 recovery remains per slot. The Server sees each slot as one Worker with
`slots=1`, one TCP connection and one active execution journal. `machine_id` and
`slot_id` are registration fields, not substrings inferred from worker_id.
The Python slot manager starts independent `forge_worker.py` processes; each has
its own complete GalaxCore SVN checkout, output directory, identity lock, outbox,
execution guard and cleanup receipts. The shared ZIP directory is read-only.

## Configuration on each Worker machine

Set these keys in the common config; each machine must use a different
`worker.machine_id`. IDs contain 1-64 letters, digits, underscores or hyphens;
`machine_id-slot_id` must also fit 64 characters.

```ini
worker.machine_id=novarch4
worker.slot_ids=slot-01,slot-02
worker.slots_root=./worker/workers_slots
worker.output_root=./worker-output
worker.checkout_parallelism=1
worker.svn_url=http://example.invalid/svn/galaxcore/galaxcore
worker.artifact_root=/path/to/shared/zip
network.server_ip=127.0.0.1
server.port=18080
```

Replace the example URL, path and IP with actual local configuration. The
manager accepts `worker.checkout_parallelism=1`; the shared checkout lock
serializes cold SVN checkouts. Existing complete checkouts are reused. Partial
checkouts are retained for inspection, never automatically deleted.

Start on the Worker host:

```bash
python3 -B -u worker/slot_manager.py
```

For an offline/demo check:

```bash
python3 -B -u worker/slot_manager.py --demo --demo-seconds 5
```

The resulting directories have the shape
`worker/workers_slots/<machine>-<slot>/galaxcore` and
`worker-output/<machine>/<slot>`. Revision in the task selects the GalaxCore
ZIP; `worker.svn_revision` fixes the checkout revision when one is configured.
The two revisions have different meanings. Never update a live slot checkout.

## Scheduling and lifecycle

The scheduler preserves priority/FIFO task ordering and rotates equally free
slots. `QUERY_STATUS` includes each Worker ID, machine, slot, status, capacity,
usage, and an aggregate per machine. A slot in `DRAINING` accepts no new task;
its current case and pending result finish normally. To remove a slot, edit
`worker.slot_ids` and signal the manager with `SIGHUP`. Re-adding a slot after
removal starts a fresh Worker under its persistent identity and output path.

Unexpected Worker exits restart with bounded backoff. Five rapid exits
quarantine the slot until the manager is restarted. A manager crash does not
kill its already-running slot processes. A restarted manager recognizes their
OS identity locks and waits; it does not create duplicate Workers. Process
supervision of those inherited processes requires an external service manager
or a later adoption mechanism. A normal manager SIGTERM requests drain for
all children and waits until active cases complete.

## Acceptance

Local fault tests use two logical machines and four independent slot processes:
four tasks run in parallel, a fifth queues, one slot crash retries safely,
draining does not interrupt a running task, and manager restart does not
duplicate already-running Workers. A true two-host intranet GalaxCore run is
still required to confirm filesystem mounts, SVN access, resource limits and
Vivado behaviour on real machines. Begin with one case on each of two slots,
then expand to two machines with two slots each.
