#!/usr/bin/env bash
set -euo pipefail

if ! command -v python3 >/dev/null 2>&1; then
    echo "[ERROR] python3 was not found in PATH. Install Python 3.8 or newer." >&2
    exit 127
fi

if ! python3 -c "import sys; raise SystemExit(0 if sys.version_info >= (3, 8) else 1)"; then
    echo "[ERROR] Python 3.8 or newer is required." >&2
    exit 1
fi

script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
project_root="$(cd -- "${script_dir}/../.." && pwd)"
exec python3 "${project_root}/build/prepare/prepare.py" "$@"
