#!/usr/bin/env bash
set -euo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
build="${BOOT_TEST_BUILD_DIR:-$root/build/boot-host}"
cmake -S "$root/tests/boot" -B "$build" \
    -DCMAKE_BUILD_TYPE=Debug -DBOOT_SANITIZERS=ON "$@"
cmake --build "$build" --parallel
ctest --test-dir "$build" --output-on-failure
