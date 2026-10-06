#!/usr/bin/env bash
# Evaluate the Ethernet link diagnostic, not frame or network operation.
set -euo pipefail

SCRIPT_DIRECTORY=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
exec python3 "${SCRIPT_DIRECTORY}/ethernet_link_test.py" "$@"
