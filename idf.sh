#!/usr/bin/env bash
set -euo pipefail

project_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

if ! command -v idf.py >/dev/null 2>&1; then
    if [[ -n "${IDF_PATH:-}" && -f "${IDF_PATH}/export.sh" ]]; then
        source "${IDF_PATH}/export.sh"
    else
        printf '%s\n' 'Activate ESP-IDF v6.0.2 first: source /path/to/esp-idf/export.sh' >&2
        exit 1
    fi
fi

cd "${project_dir}"
exec idf.py "$@"
