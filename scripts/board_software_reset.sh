#!/usr/bin/env bash
# Reset the EK-LM3S6965 and resume execution via OpenOCD SYSRESETREQ.
set -euo pipefail

if [[ $# -gt 0 ]]; then
    if [[ $# -eq 1 && ( "$1" == "-h" || "$1" == "--help" ) ]]; then
        printf '%s\n' \
            'Usage: scripts/board_software_reset.sh [-h|--help]' \
            '' \
            'With no arguments, reset the EK-LM3S6965 and resume installed firmware' \
            'via OpenOCD SYSRESETREQ and reset run; no flash programming.' \
            'OpenOCD target reset hooks can govern the actual reset sequence.' \
            'Requires separate authorization for a live reset.' \
            '' \
            '-h, --help: show this help without accessing hardware.' \
            'All other arguments (including --dry-run) and extra arguments are rejected.'
        exit 0
    fi
    printf '%s\n' \
        'error: unsupported arguments; use no arguments to reset, or -h/--help alone for help.' >&2
    exit 2
fi

REPOSITORY_ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
exec openocd -f "${REPOSITORY_ROOT}/tools/openocd/board/ek-lm3s6965.cfg" \
    -c init -c 'cortex_m reset_config sysresetreq' -c 'reset run' -c shutdown
