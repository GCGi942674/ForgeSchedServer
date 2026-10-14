# Daily regression by date and template

Clock resolves `Tasks.yaml` entries using `server.templates_dir/<template>.json`.
The directory remains configurable; templates not referenced by Tasks.yaml do not run automatically.
Each newly planned task retains its flow_config snapshot and context:
`source=scheduled`, `date=YYYY-MM-DD`, `template`, `suite`, `name`, `batch_id`.
The date is the scheduling machine's local calendar date captured during planning,
not a Worker timestamp or execution-completion date. Resume retains the original plan date.
Existing SQLite Task payload persistence stores these fields; no schema migration is required.

## Query contract

The existing `query_regression` operation supports:

```json
{"date":"2026-10-12","baseline_date":"2026-10-11","template":"route","limit":50}
```

Optional `search` matches a substring of the testcase path. Optional `change` accepts
REGRESSION, IMPROVEMENT, MISSING, EXCEPTION, UNCHANGED. Omit template for all stages.
Only records explicitly tagged `source=scheduled` with a valid date and template are eligible.
Manual and unclassified historical tasks are excluded; old Clock plans are not retroactively relabeled.
`excluded_unclassified_or_manual` is an inventory-wide diagnostic count, not a per-day total.

Rows align by `(template, case)` and contain baseline and comparison Task details.
PASS to FAIL means REGRESSION; FAIL to PASS means IMPROVEMENT.
Missing sides remain MISSING. TIMEOUT, CRASH, ERROR, ACTIVE, CANCELLED, UNKNOWN on
the comparison side remain EXCEPTION, never silently treated as FAIL.
Other pairs are UNCHANGED, meaning no confirmed PASS-to-FAIL regression, not necessarily identical outcomes.
Duplicate same-day/template/case submissions select the greatest task_id, with duplicate_records reported.
Task results and IDs remain available; no attempt is deleted by this projection.

Responses provide counts, stage-level outcome/change summaries, templates, items,
has_more, next_after_key, snapshot_generation. Repeat the same filters and pass
next_after_key as after_key to fetch another page (maximum 50 rows per page).
Stage summaries ignore search/change narrowing; total and counts apply those filters.
GUI rejects a next page if the Server instance or task generation changed; use Compare/Home again.
Adding date alone queries scheduled Tasks for that date using the existing before_task_id pagination.

## Qt UI and deployment

The live Daily Regression page supports dates, template stage, change filter, case search,
Compare/Home, Next Page, and baseline/comparison Task/log detail actions.
Old Servers reject the new query; errors are shown explicitly, not replaced with demo data.
Deploy updated Server, Clock, Worker adapter, and Qt GUI together. Stop/drain and restart
using the existing deployment procedure, retain the SQLite DB and slot directories.
Older rows without source/date are intentionally not included. A future historical backfill
requires separately verified provenance; no automatic guessing is performed.

Local checks: test_regression_service covers stage isolation, manual exclusion, date validation,
missing/timeout classification, duplicate selection, and a 4000-case paginated inventory.
Qt tests and protocol tests must also pass. Real intranet daily-batch verification remains separate.
