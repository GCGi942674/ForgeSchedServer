# GalaxCore Regression Platform naming migration

Product: **GalaxCore Regression Platform**, abbreviated **GCRP**.
Repository: https://github.com/GCGi942674/GalaxCoreRegressionPlatform
Local development checkout: `/home/zhenlei/projects/GalaxCoreRegressionPlatform`.

| Old identifier | New identifier |
| --- | --- |
| ForgeSched namespace | GCRP |
| ForgeClient class/header | GcrpClient / GcrpClient.h |
| forgesched_client | gcrp_client |
| server executable / CMake target | gcrp_server |
| forgesched_gui | gcrp_gui |
| worker/forge_worker.py | worker/gcrp_worker.py |
| config/forgesched.conf | config/gcrp.conf |
| FORGESCHED_CONFIG | GCRP_CONFIG |
| FORGESCHED_MACHINE_NAME | GCRP_MACHINE_NAME |
| FORGESCHED_BUILD_QT_GUI | GCRP_BUILD_QT_GUI |
| FORGESCHED_JSON_INCLUDE_DIR | GCRP_JSON_INCLUDE_DIR |

## Existing deployments

This is a coordinated source/deployment rename, not a database migration.
TCP framing, JSON message types, Task IDs, SQLite table definitions and schema
versions remain unchanged. Existing database files, runtime journals, reports,
log archives, Worker identity files, SVN copies and reference sources are not rewritten.

1. Let active real cases finish, deliver pending results/logs, and stop Clock,
   GUI, Worker managers and Server gracefully. Do not remove runtime journals.
2. Retain a backup of the existing SQLite database and runtime configuration.
   Do not copy an active database without its WAL or a proper SQLite backup.
3. Carry the current deployment settings into `config/gcrp.conf`.
   **Keep database.path pointing to the existing database** if it has the old
   filename. The new-install default `./data/gcrp.db` is not an automatic migration:
   changing to it without moving/backing up the previous database starts a different DB.
   Retain worker.output_root, worker.slots_root and log archive paths to reuse data.
4. Update environment variables and service/cron commands to the new names.
   No legacy configuration or executable-name alias is provided.
5. Reconfigure and rebuild; cached CMake source paths cannot survive a moved checkout.
   A production checkout may keep its existing folder path if needed to preserve
   absolute deployment/journal paths. Update directory references explicitly if moving it.
6. Restart Server, Worker managers, Clock and GUI using matching renamed versions.

Typical commands from the checkout root:

```bash
make
./build/bin/gcrp_server
./build/bin/gcrp_client workers
python3 -B -u worker/slot_manager.py
python3 -B -u server/clock.py --check-only
python3 -B -u tools/intranet_qt_smoke.py --show-demo
./build/gui/bin/gcrp_gui --ip 192.168.10.11 --port 8080
```

With `-DGCRP_BUILD_QT_GUI=ON` in the main build, GUI is instead `build/bin/gcrp_gui`.
The dedicated Qt smoke helper builds its standalone GUI under `build/gui/bin`.

GitHub repository identity/history were retained when renaming. Local origin was
updated to the new URL; this does not commit or push the uncommitted source changes.
Existing archived records may still contain the former branding, intentionally.
