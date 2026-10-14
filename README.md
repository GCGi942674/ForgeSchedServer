# GalaxCore Regression Platform

C++17 scheduling server with a v0.1 TCP/JSON control plane and a minimal CLI.
The platform is abbreviated GCRP; its C++ namespace and build/configuration identifiers use this name.
Development and tests run in WSL; the internal deployment consumes this code.

- [Control plane, CLI usage and protocol boundaries](docs/control-plane-v0.1.md)
- [Build instructions and regression coverage](tests/README.md)
- [PJtest integration review and remaining work](docs/pjtest-integration-phase6.md)
- [PJtest Adapter configuration and offline acceptance](docs/pjtest-adapter.md)
- [Product rename and existing deployment migration](docs/rename-migration.md)
- [Daily regression by date and template](docs/daily-regression.md)
- [C++ / Qt5 desktop console](docs/qt5-client.md)

Build with `make`; run `./build/bin/gcrp_server` from the repository root.
Use `./build/bin/gcrp_client --help` for submit/query/cancel commands.

A Python Worker supports isolated SVN slots and PJtest execution. SQLite state
persistence, recovery, regression queries, log archiving and a Qt5 GUI are available.
Cancellation stops queued cases; remote termination of active real processes remains unsupported.
Roles are not authentication:
use only on trusted networks, not a public Internet endpoint.
