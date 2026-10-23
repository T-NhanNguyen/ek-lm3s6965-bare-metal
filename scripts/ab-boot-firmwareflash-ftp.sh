#!/usr/bin/env bash
# Host FTP demo entry point; no OpenOCD or device-root provisioning.
set -euo pipefail
SCRIPT_DIRECTORY=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
REPOSITORY_ROOT=$(dirname "$SCRIPT_DIRECTORY")
export PYTHONPATH="${REPOSITORY_ROOT}${PYTHONPATH:+:${PYTHONPATH}}"
exec python3 -m tools.boot.demo "$@"
