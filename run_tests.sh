#!/usr/bin/env bash
# Compiler errors, missing binaries, timeouts and nonzero test exits all fail.
set -euo pipefail
ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
export ARCHITECTURE="${ARCHITECTURE:-x86}"
while (($#)); do case "$1" in -Architecture|--architecture) ARCHITECTURE="${2:?Architecture value required}"; shift 2 ;; *) echo "Unknown argument: $1" >&2; exit 1 ;; esac; done
case "$ARCHITECTURE" in x86|x64) ;; *) echo "Architecture must be x86 or x64" >&2; exit 1 ;; esac
export BUILD_DIR="${BUILD_DIR:-$ROOT/build/$ARCHITECTURE}"
export CONFIGURATION="${CONFIGURATION:-Release}"
export MODE=Tests
if [[ "${NO_BUILD:-0}" != 1 ]]; then bash "$ROOT/build.sh"; fi
CTEST="${CTEST:-ctest}"
command -v -- "$CTEST" >/dev/null 2>&1 || { echo 'CTest missing; set CTEST.' >&2; exit 1; }
[[ -f "$BUILD_DIR/CTestTestfile.cmake" ]] || { echo 'CTest test manifest missing; build with BUILD_TESTING=ON.' >&2; exit 1; }
"$CTEST" --test-dir "$BUILD_DIR" -C "$CONFIGURATION" --output-on-failure --no-tests=error --timeout "${TEST_TIMEOUT:-120}"
