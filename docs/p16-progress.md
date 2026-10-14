# P16 project progress and rollback detection

P16 reads the Scheduler's restored Task state. It does not create another
database or infer a result from a process exit code. A case is identified by
its `case` path and `flow` within one `target`. At each numeric revision, the
Task with the greatest task ID is the latest attempt for that case. Other
Task types and non-numeric revisions do not enter the progress view.

`QUERY_PROGRESS` requires `target`; optional fields are `flow`, `limit` (1–20)
and `before_revision` (numeric). The response lists revisions newest first,
with the same detailed outcome counts as Regression queries, new and missing
cases, and rollback/fix counts against the immediately preceding observed revision.
The `next_before_revision` cursor pages through older revisions. A case absent
in either revision is not counted as a rollback.

`QUERY_ROLLBACK` requires `target`, `from_revision`, `to_revision` with
`from_revision < to_revision`; optional fields are `flow`, `limit` (1–50) and
`after_task_id`. It returns cases whose latest Task in the earlier revision
was `SUCCEEDED` with `case_status=PASS` and whose latest Task in the later
revision was `FAILED` with `case_status=FAIL`. The response contains both
task IDs and an `after_task_id` cursor. A timeout, crash, missing `result.env`,
cancellation, or active execution cannot be called a business rollback.

CLI examples:

```sh
./build/bin/gcrp_client progress --target route --limit 10
./build/bin/gcrp_client rollback --target route --from 18925 --to 18926
```

The comparison is based on the latest Task ID, not wall-clock timestamps, so
different Worker clocks do not affect it. It is a read-only projection and
can be recomputed after Server restart. The current implementation scans the
in-memory Task snapshot; performance and API freezing belong to later stages.
