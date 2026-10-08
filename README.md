# ForgeSched

C++17 scheduling server with a v0.1 TCP/JSON control plane and a minimal CLI.
Development and tests run in WSL; the internal deployment consumes this code.

- [Control plane, CLI usage and protocol boundaries](docs/control-plane-v0.1.md)
- [Build instructions and regression coverage](tests/README.md)
- [PJtest integration review and remaining work](docs/pjtest-integration-phase6.md)

Build with `make`; run `./build/bin/server` from the repository root.
Use `./build/bin/forgesched_client --help` for submit/query/cancel commands.

A [single-slot Python demo Worker](docs/python-worker.md) now runs real child
processes. It is not yet a PJtest/Vivado executor. No persistence,
remote process cancellation or GUI is implemented. Roles are not authentication:
use only on trusted networks, not a public Internet endpoint.
