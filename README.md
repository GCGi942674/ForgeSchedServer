# GalaxCore Regression Platform

C++17 scheduling server, Python PJtest Workers and a C++/Qt5 desktop console over TCP/JSON.
The platform is abbreviated GCRP; its C++ namespace and build/configuration identifiers use this name.
Development and tests run on Linux (currently the Debian development VM); the internal deployment consumes this code.

- [Server API and protocol boundaries](docs/server-api-v1.md)
- [Build instructions and regression coverage](tests/README.md)
- [PJtest Adapter configuration and offline acceptance](docs/pjtest-adapter.md)
- [Product rename and existing deployment migration](docs/rename-migration.md)
- [Daily regression by date and template](docs/daily-regression.md)
- [C++ / Qt5 desktop console](docs/qt-ui-v3.md)
- [Start, stop and status scripts](docs/service-scripts.md)
- [Worker management](docs/worker-controls.md)
- [GUI catalog and troubleshooting](docs/gui-catalog.md)
- [Intranet acceptance](docs/intranet-full-acceptance.md)

Build with `make`; run `./build/bin/gcrp_server` from the repository root.
Use `./build/bin/gcrp_client --help` for submit/query/cancel commands.

A Python Worker supports isolated SVN slots and PJtest execution. SQLite state
persistence, recovery, regression queries, log archiving and a Qt5 GUI are available.
Cancellation stops queued cases; remote termination of active real processes remains unsupported.
Roles are not authentication:
use only on trusted networks, not a public Internet endpoint.

## 配置与目录更新

配置仅拆为 `config/server.conf`（Server/CLI/GUI/每日提交）与 `config/worker.conf`（Worker）。旧 `gcrp.conf` 和显式 `GCRP_CONFIG` 继续兼容。
现有部署从项目根运行 `python3 -B tools/split_config.py`；工具不会覆盖目标，也不会搬数据库、日志、身份文件或 SVN 副本。新部署使用对应 `.conf.example`。
Qt 代码已整理为 `gui/qt/include` 与 `gui/qt/src`；构建入口不变。完整路径及日志说明见 [目录整理](docs/layout-plan.md)。
