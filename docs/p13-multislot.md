# P13 multiple Workers and isolated slots

P12 recovery remains per slot. The Server sees each slot as one Worker with
`slots=1`, one TCP connection and one active execution journal. The Python slot
manager starts an independent `forge_worker.py` process for each slot. Each
has its own complete GalaxCore SVN checkout, output directory, identity lock,
outbox, execution guard and cleanup receipts. The shared ZIP directory is
read-only.

## Worker configuration

Configure the Server address, SVN URL and artifact location for each Worker
machine. Only the slot **count** is configured; names are generated from the
machine's hostname:

```ini
worker.slots=2
worker.slots_root=./worker/workers_slots
worker.output_root=./worker-output
worker.checkout_parallelism=1
worker.svn_url=http://example.invalid/svn/galaxcore/galaxcore
worker.artifact_root=/path/to/shared/zip
network.server_ip=127.0.0.1
server.port=18080
```

Start with `python3 -B -u worker/slot_manager.py`. The manager creates and
registers `<machine>-slot1`, `<machine>-slot2`, etc. Existing complete SVN
checkouts are reused. Partial checkouts are retained for inspection, never
automatically deleted. The task revision selects the GalaxCore ZIP;
`worker.svn_revision`, when set, pins the checkout revision. Those revisions
have different meanings. Never update a live slot checkout.

The offline demo is `python3 -B -u worker/slot_manager.py --demo --demo-seconds 5`.

## Scheduling and lifecycle

The scheduler preserves priority/FIFO task ordering and rotates equally free
slots. `QUERY_STATUS` includes each Worker ID, machine, slot, status, capacity,
usage, and an aggregate per machine. A slot in `DRAINING` accepts no new task;
its current case and pending result finish normally. Change `worker.slots` and
signal the manager with `SIGHUP` to grow or shrink. Removed slots retain their
SVN copies, so increasing the count later reuses them.

Unexpected Worker exits restart with bounded backoff. Five rapid exits
quarantine the slot until the manager is restarted. A manager crash does not
kill its already-running slot processes. A restarted manager recognizes their
OS identity locks and waits; it does not create duplicate Workers. Process
supervision of those inherited processes requires an external service manager
or a later adoption mechanism. A normal manager SIGTERM requests drain for
all children and waits until active cases complete.

See [Worker slot count and automatic registration](worker-slots.md) for the
identity file, legacy SVN-copy reuse and migration preview. A true two-host
intranet GalaxCore run is still required to confirm mounts, SVN access,
resource limits and Vivado behavior on real machines.
