# Current-stage regression tests

These tests are maintained for the external mirror. They do not claim to be a
copy of the unavailable internal Claude Code acceptance suite.

The synchronized milestone remains the scheduling core plus WorkerSession /
WorkerServer network integration. No client submission endpoint, automatic
scheduling, persistence, real executor, or recovery feature is added here.

## Run on Linux / WSL

From the repository root (C++17 compiler, CMake >= 3.15.2 and pthreads required):

```sh
make debug
make test BUILD_TYPE=Debug
(cd build && ctest -R 'test_worker_network|test_server_process' --repeat-until-fail 30 --output-on-failure)
```

JSON is supplied by `third_party/nlohmann/json.hpp`. Build directories can be
removed and regenerated. No Python or separate verification directory is needed.

Address/undefined-behavior checks (use a separate build directory):

```sh
cmake -S . -B build/sanitized -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_CXX_FLAGS=-fsanitize=address,undefined \
  -DCMAKE_EXE_LINKER_FLAGS=-fsanitize=address,undefined
cmake --build build/sanitized -j4
(cd build/sanitized && ctest --output-on-failure)
```

Validated on 2026-09-23 with WSL Ubuntu / GCC 13.3:

- Fresh Debug build from the Windows-mounted checkout: 8/8 tests passed.
- Worker network and server-process tests: 30 consecutive passes each.
- Fresh ASan + UBSan build of a source snapshot on native Linux storage: 8/8
  passed, with no sanitizer diagnostics. This also verifies case-sensitive
  header lookup; it is not a migration of the working repository.
- Real PJTest/Vivado execution and production deployment remain unverified.

## Coverage

| Test | Checks |
| --- | --- |
| test_logging | Module tags, level filtering, file routing, error duplication, concurrent writes, source basename |
| test_thread_pool | Existing thread-pool tests |
| test_EchoClient | Existing Echo client compatibility tests |
| test_EchoServer | Existing 100-client Echo transport test |
| test_scheduler | Complete transition matrix, priority/FIFO, slot ownership, failed/throwing dispatch rollback, cancellation, duplicate completion, re-registration, large capacity, concurrent ID generation |
| test_protocol | Partial/coalesced frames, size cap, malformed envelopes, integer boundaries, Router submit/query/cancel, Config parsing |
| test_worker_network | Real framed TCP traffic, role/identity checks, stale connection replacement, assignment/start/result, disconnect reservations, half-close, weak registry ownership, unavailable dispatcher, live-connection shutdown |
| test_server_process | Actual server executable, registration, SIGTERM/SIGINT, invalid configuration, occupied-port startup failure |

New checks remain active in Release builds. Existing assert-based tests are built
with assertions enabled. Socket operations have deadlines; the process fixture
reaps its child on failure. Temporary configuration/log directories are isolated
and removed by their fixtures. The new network tests use dynamically selected
loopback ports; the brief bind/rebind interval can still race with an unrelated
process. Legacy Echo tests retain their existing fixed ports.

## Boundaries to preserve for the next internal sync

- `WorkerServer::runOnce()` is invoked explicitly by the integration test, not
  automatically by production `main`.
- A successful dispatch means acceptance into the connection's owner-loop queue,
  not confirmation that the remote worker received or executed it.
- Disconnecting does not release ASSIGNED/RUNNING reservations or retry tasks.
- Cancellation tests pin the current local state/slot behavior; they do not prove
  a remote executor has stopped. Remote cancellation acknowledgement remains a
  prerequisite before real execution can safely reuse that capacity.
- Allocation-failure exception safety and production-scale endurance are not
  established by ordinary rollback and smoke tests.
- Mutable WorkerServer service getters are for controlled setup/inspection; a
  future concurrent control endpoint must honor the host serialization boundary.
- The historical master MD is absent from this checkout. Compare the next
  internal sync against its current authoritative MD before advancing milestones.

## Build entry point and internal toolchain

`make` builds Release; `make debug` builds Debug. Both default directly to `build`.
The default parallel job count is 32; override `PARALLEL_JOBS` if needed.
`make clean` cleans generated targets there but retains the cache; it succeeds
without doing anything when that directory has not been configured. Executables
are placed in `build/bin` and libraries in `build/lib`.

The Makefile prefers `/home/xshare/scripts/bin/cmake-3.15.2/bin/cmake` when that
file exists, otherwise it uses `cmake` from PATH. Interactive shell aliases are
not inherited by make. Explicit selection is always supported:

```sh
make CMAKE=/home/xshare/scripts/bin/cmake-3.15.2/bin/cmake
make test CMAKE=/home/xshare/scripts/bin/cmake-3.15.2/bin/cmake
make clean CMAKE=/home/xshare/scripts/bin/cmake-3.15.2/bin/cmake
```

CTest defaults to the executable beside the selected CMake; override `CTEST`
if necessary. The commands avoid newer CTest `--test-dir` and `--repeat` syntax.
The former `build/linux` and `build/cmake315` directories were verification
artifacts, not required project layouts. Normal builds do not create them.
Never reuse caches copied from another machine/path.
The CMake baseline does not imply compiler/standard-library compatibility:
the project still requires C++17, including `std::filesystem` support.

Compatibility validation (2026-09-23, WSL Ubuntu / GCC 13.3):

- System CMake/CTest 3.28.3: clean, Release rebuild, default make and 8/8 tests passed.
- Official CMake/CTest 3.15.2: fresh Release build and 8/8 tests passed; clean
  removed the executable while retaining CMakeCache.txt and source files.
- clean on an unconfigured directory returned success without creating/removing files.
- The 3.15.2 Linux archive was checked against Kitware's published SHA256 list;
  it was used from a temporary directory, without replacing system CMake.
- This validates CMake compatibility, not the as-yet-unverified internal compiler,
  standard library, operating system or production runtime.

Reference: https://cmake.org/cmake/help/v3.15/manual/cmake.1.html

## WSL location

The authoritative working copy is `/home/zhenlei/projects/ForgeSched` in WSL
Ubuntu. `/mnt/d/study/Vivado_test/Server` is the retained Windows backup; it is
not updated in parallel. Configure a fresh build directory after moving sources.
