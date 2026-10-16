# Regression tests

Run from the repository root on Linux with a C++17 toolchain, SQLite3 development
files, CMake >= 3.15.2 and Python >= 3.6.8. Qt5 Widgets/Network enables GUI tests.

```sh
make PARALLEL_JOBS=2
(cd build && ctest --output-on-failure)
```

Use `PARALLEL_JOBS=1` on low-memory hosts. For an explicit intranet toolchain:

```sh
make CMAKE=/home/xshare/scripts/bin/cmake-3.15.2/bin/cmake PARALLEL_JOBS=2
(cd build && /home/xshare/scripts/bin/cmake-3.15.2/bin/ctest --output-on-failure)
```

Run CTest inside the build directory: CTest 3.15 does not support `--test-dir`.
Never copy a CMake cache between machines. Release tests retain assertions with
`-UNDEBUG`: setup inside legacy assertions must not be compiled away.

Coverage includes transport and scheduling, concurrent submissions, queued-case
cancellation, SQLite persistence, restart/recovery, multi-slot identity and
distribution, regression/progress queries, PJtest mapping, evidence archives,
retention, Worker maintenance, and Qt transport/widgets/catalog integration.
The registered suite in CMake is the authoritative list, not historical counts.

Tests use isolated fake executables, temporary databases and loopback services.
They do not prove real GalaxCore execution or desktop rendering in the intranet.
Do not replace fixture revisions or paths with production settings. Existing
Echo tests validate network infrastructure and are intentionally retained.

- [Intranet acceptance](../docs/intranet-full-acceptance.md)
- [PJtest Adapter](../docs/pjtest-adapter.md)
- [Server API](../docs/server-api-v1.md)
- [Qt console and visual checks](../docs/qt-ui-v3.md)

Keep test sources for future regression runs. Generated build outputs, runtime
databases, Worker output and intranet reports are ignored, not release content.
