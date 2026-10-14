# P15 Regression query cache

The optional cache stores complete `QUERY_REGRESSION` responses by filter and
pagination cursor. A repeated query can return without copying and sorting all
Scheduler tasks. It is disabled by default. Enable it in `config/gcrp.conf`:

```ini
cache.regression.enabled=true
cache.regression.max_entries=1000
cache.regression.ttl_seconds=5
```

Each committed task change advances the Scheduler task generation. A query
captures tasks and generation under one lock, computes its response, and
stores it only if the generation is still current. A changed generation clears
older entries. A cache allocation failure falls back to the computed result.
Restart begins with an empty cache. `QUERY_STATUS.regression_cache` reports
hits, misses, invalidations and live entries.

Task, run and batch state continue to use the synchronous StateStore transaction.
The current schema has no non-critical progress or log record with an existing
reader. Adding an asynchronous SQLite queue now would create unused persisted
data and a second writer competing with task commits. Introduce it when a
specific consumer and loss policy are defined; do not enqueue Task or TaskRun
transitions.

Validation: `test_regression_service` covers repeated reads and invalidation
after submit, cancel and completion; the existing persistence, recovery and
control-plane suites cover the unchanged durable path. No throughput claim is
made until measurements on a representative workload show a benefit.
