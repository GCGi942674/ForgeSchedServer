# C++ / Qt5 desktop client — first native version

> Historical first-version notes. Current behavior and build instructions are in
> [Qt console v3](qt-ui-v3.md). Live startup now requires
> `gcrp_gui --ip 192.168.10.11 --port 8080`; the GUI no longer reads configuration files.

The GUI is optional. Default Server builds do not require Qt. It has a standalone
CMake 3.15.2 entry in `gui/qt`, requiring C++17 and Qt 5.8+ Core/Widgets/Network.
No browser engine, QML, Python GUI, SQLite or Server binaries are needed to build it.

## Implemented and pending

- Native sidebar and four pages: Workers, Tasks, Add Task, Daily Regression.
- Live mode: paginated worker/task queries, machine/slot grouping, task detail,
  cancel task/batch, finished-run log file list and bounded hex-decoded text pages.
  Requests are asynchronous; failed refresh retains last known data with a warning.
  Task counters and Worker association are monitoring snapshots, not a joined DB transaction.
  Lists currently stop at 1000 rows and explicitly report partial loading.
- Task selection uses tri-state directory checkboxes. Since the Server has no
  directory-catalog API, export a snapshot on the Server and import it into the GUI.
  Human submission is sequential and reports partial/uncertain results; it never
  silently retries. `batch_id` and per-case submission keys are generated once per attempt.
  On uncertainty, query the batch before submitting again; the GUI does not yet
  persist a draft/retry journal across restarts. The imported snapshot may become stale;
  Server/Worker validation remains authoritative.
- Logs use task_id + current run_id. Paging is 8 KiB by default; search applies
  only to the displayed page. Missing archive files cannot yet be distinguished
  as pending/not-captured/expired by the API, so the UI lists these possibilities.
  Historical-attempt selection, byte-exact download and full-file search are pending.
- Daily date comparison is demonstrable in `--demo`. Live comparison is explicitly
  disabled until a dedicated daily-source/date API exists. Manual batches are not
  inferred to be daily regressions from an arbitrary name or timestamp.
- Remote machine/slot controls and record deletion are visibly disabled pending APIs.
  They do not simulate successful live operations.

## Tomorrow's intranet checks

On the machine with a desktop/remote X session (GUI is not needed on every Worker):

```sh
python3 -B -u tools/intranet_qt_smoke.py
python3 -B -u tools/intranet_qt_smoke.py --show-demo
python3 -B -u tools/intranet_qt_smoke.py --connect
```

The first command builds and runs socket/widget tests with an offscreen Qt platform.
`OFFSCREEN_PASS` does not establish that the X11/xcb desktop can open a window.
The second opens the demo; close it to finish and produce `PASS`.
Reports: `intranet-report/qt-gui/report.json` and configure/build/test/display logs.
`--connect` also performs read-only capability/worker/task queries against the
configured running Server. It does not submit/cancel any task. An OFFSCREEN_PASS
with this option includes a successful `server_probe` entry, but still does not
prove visible desktop compatibility. Chinese square boxes indicate missing fonts;
install a Chinese system font on the GUI machine.
If Qt CMake packages cannot be found, set `gui.qt_prefix` to the actual Qt install
prefix (for example the directory containing `lib/cmake/Qt5`). Qt headers and dev
packages are required; having a GUI runtime alone is insufficient.
Do not mix a different Qt version's libraries/plugins through LD_LIBRARY_PATH or
QT_PLUGIN_PATH. A headless Server terminal cannot show a desktop window without a
working DISPLAY/Wayland session. Windows native Qt5 builds are also supported by
the standalone project; build with the matching MSVC or MinGW Qt package.

The existing stage acceptance is independent:

```sh
python3 -B -u tools/intranet_full_acceptance.py --role server --scale-host novarch4
# Then run the printed Worker command on novarch3 and novarch4.
```

Keep fake fixture revision `58231` in tests and `tests/fixtures/pjtest/setup.py`.
Only `smoke.revision` chooses the real ZIP for stage acceptance. Deploy the fixed
`tests/test_pjtest_adapter.cpp` so isolated tests ignore production Worker paths.

## Catalog and real connection

Add these optional fields to the existing shared config (all existing Server/Worker
settings stay as currently deployed):

```ini
gui.catalog_file=./intranet-report/gui-catalog.json
gui.refresh_seconds=5
gui.log_page_bytes=8192
gui.qt_prefix=
tools.qt_qmake=
```

On the Server, configure its real `server.work_root`, `server.templates_dir` and
`server.artifact_root`, then run:

```sh
python3 -B tools/export_gui_catalog.py
```

Copy `intranet-report/gui-catalog.json` to the GUI machine and choose Import Catalog
or configure `gui.catalog_file`. The exporter excludes symlinks and hidden dirs,
limits the file to 16 MiB and cases to 50000. It writes only the chosen catalog;
it does not modify the SVN checkout or submit tasks. No directory path is sent
as a shell command. The GUI's target field remains explicit rather than guessing
device metadata for arbitrary directory structures.

```sh
./build/gui/bin/gcrp_gui --ip 192.168.10.11 --port 8080
```

The address and timeout come from `network.server_ip`, `server.port` and
`network.timeout_ms`. `--demo` needs neither config nor live Server.

## Manual build

```sh
cmake -S gui/qt -B build/gui -DBUILD_TESTING=ON
cmake --build build/gui --parallel 8
cd build/gui
ctest --output-on-failure
```

For a combined build, opt in with `-DGCRP_BUILD_QT_GUI=ON`; otherwise Qt is
never searched. The standalone build avoids the Linux-only Server targets.
