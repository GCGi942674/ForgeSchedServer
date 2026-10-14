# One-entry intranet acceptance

Use the same GCRP revision and `config/gcrp.conf` on the Server
and both Worker machines. As in the earlier acceptance run, set
`smoke.revision`, `smoke.case`, `smoke.target`, the Server address, artifact
directory and Worker SVN URL for real PJtest execution.

For a single-machine check without GalaxCore or running Workers:

```sh
python3 -B -u tools/intranet_full_acceptance.py --offline-only
```

This configures, builds and runs the complete CTest suite serially. It checks
P10 through P17 and the Server/Worker log archive and retention tests. The
result is `OFFLINE_PASS` only when every registered test passes.

For the real three-machine acceptance, start on the Server:

```sh
python3 -B -u tools/intranet_full_acceptance.py --role server --scale-host novarch4
```

When it prints a one-time token, start the same script on **each** Worker
machine, substituting the printed token:

```sh
python3 -B -u tools/intranet_full_acceptance.py --role worker --token TOKEN
```

The Server first runs the P15–P17 and log tests against isolated fixtures,
then the existing P10–P14 controller drives the real PJtest case, Server
restart, batch cancellation and 3→5 slot expansion. The Worker role delegates
to the existing acceptance agent. A Worker does not need a C++ build. Use
`--no-build` on the Server only when the full test suite is already built.

The concise result is in `intranet-report/full-acceptance/report.json` on the
Server. `configure.log`, `build.log` and `ctest-*.log` are there only for
diagnostics. The real-case report remains under `intranet-report/p10-p14-*/`.
A result of `PASS` includes live P10–P14; `OFFLINE_PASS` only covers isolated
tests. P15–P17 and 30-day retention are validated with isolated tests, not
with a 30-day wait on the real Worker. The printed token is masked in the
saved controller console log.
