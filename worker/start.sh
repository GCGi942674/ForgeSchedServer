#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
exec "${GCRP_PYTHON:-python3}" "$ROOT/tools/service_control.py" worker start "$@"
