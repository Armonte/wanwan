#!/usr/bin/env bash
# Build + run the host-native unit tests (Linux gcc, no SDL / no Windows /
# no Wine). Fast iteration loop for wire format, queue mechanics, delay math
# and the reliable-channel layer -- does NOT exercise the actual hook DLL
# (that's the mingw-cross path used by build.sh / go.sh).
#
# Wired into tools/run_all_tests.sh as the "unit" stage. Exits non-zero if
# any target fails to build or any test fails, so it gates cleanly.
set -euo pipefail
cd "$(dirname "$0")"
mkdir -p build
cd build
cmake .. -G Ninja -DCMAKE_BUILD_TYPE=Debug "$@"
ninja

rc=0
echo "=== fm2k_tests (doctest) ==="
./fm2k_tests --reporters=console || rc=$?
echo "=== delay_math_test ==="
./delay_math_test || rc=$?
echo "=== reliable_channel_test ==="
./reliable_channel_test || rc=$?

if [ "$rc" -ne 0 ]; then
    echo "unit tests FAILED (rc=$rc)" >&2
fi
exit "$rc"
