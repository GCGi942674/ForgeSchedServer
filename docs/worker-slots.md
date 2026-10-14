# Worker slot count and automatic registration

On each Worker host, configure the normal Server address, SVN URL, artifact
directory and output directories. The only slot topology setting is:

```ini
worker.slots=3
```

Start `python3 -B -u worker/slot_manager.py`. On a host named `novarch4`,
the manager starts `slot1`, `slot2`, `slot3`. Each child registers itself
with GCRP as `novarch4-slot1`, `novarch4-slot2`,
`novarch4-slot3`. Each gets its own full SVN working copy under
`worker/workers_slots/<worker-id>/galaxcore`. Existing complete copies are
reused. `worker.checkout_parallelism=1` limits SVN checkout concurrency,
not task concurrency.

At first start the manager saves a machine name and local UUID in
`worker.output_root/.machine-identity.json`. It reuses them on subsequent
starts, even if the OS hostname changes. The Server currently groups Workers
by the saved machine name. The UUID is local state reserved for a later
machine-level protocol; it is not yet sent to the Server. Give hosts distinct
hostnames. The optional `GCRP_MACHINE_NAME` environment variable can
set the name **before the first start** for a test or a duplicate hostname;
routine deployment does not need it. Do not copy the output directory,
including the identity file, to another physical Worker host.

To grow from three slots to five, change `worker.slots=5` and send `SIGHUP`
to the manager process, or restart it after a clean drain. `slot1` through
`slot3` keep their checkouts; `slot4` and `slot5` are prepared if missing.
To shrink, set a smaller count and send `SIGHUP`. Removed slots stop taking
new tasks and finish their current tasks before exiting. Their SVN copies
remain for future expansion. Normal `SIGTERM` also drains all slots.

## Existing SVN copies

Stop the old manager before changing to `worker.slots`. From the GCRP
root, preview the reuse plan:

```bash
python3 -B worker/slot_manager.py --migration-preview
```

For example, a unique complete checkout at
`worker/workers_slots/novarch4-slot-01-slot-01/galaxcore` is renamed to
`worker/workers_slots/novarch4-slot1/galaxcore`. No SVN download is needed.
The manager checks that the old manager/slot locks are free and that the old
output directory has no active or pending report before moving. Ambiguous
matches and occupied slots stop startup with a diagnostic. Existing new
targets are never overwritten; partial new targets must be inspected
manually. Historical old output logs stay in their old location. The Server
may still show old Worker IDs as OFFLINE historical records; no database
records are deleted by this migration.

The first revision after migration should be checked with one real testcase
per slot. Stop on any migration error and inspect the named directory rather
than deleting an SVN copy or a recovery journal.
