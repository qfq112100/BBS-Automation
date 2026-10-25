#!/usr/bin/env bash
# CMake wrapper for portable MinGW-w64 / LLVM-MinGW, MSYS2 and cross compilers.
# Set CXX, RC, OBJDUMP and optionally CMAKE/MAKE to exact trusted tool paths.
set -euo pipefail
ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
ARCHITECTURE="${ARCHITECTURE:-x86}"
while (($#)); do case "$1" in -Architecture|--architecture) ARCHITECTURE="${2:?Architecture value required}"; shift 2 ;; *) echo "Unknown argument: $1" >&2; exit 1 ;; esac; done
case "$ARCHITECTURE" in x86) triple=i686-w64-mingw32 ;; x64) triple=x86_64-w64-mingw32 ;; *) echo "Architecture must be x86 or x64" >&2; exit 1 ;; esac
BUILD_DIR="${BUILD_DIR:-$ROOT/build/$ARCHITECTURE}"
MODE="${MODE:-All}"
CONFIGURATION="${CONFIGURATION:-Release}"
CMAKE="${CMAKE:-cmake}"
locate() { local name; for name in "$@"; do if command -v -- "$name" >/dev/null 2>&1; then command -v -- "$name"; return 0; fi; done; return 1; }
CXX="${CXX:-$(locate ${triple}-clang++ ${triple}-g++ g++ || true)}"
RC="${RC:-$(locate ${triple}-windres windres || true)}"
OBJDUMP="${OBJDUMP:-$(locate ${triple}-objdump llvm-objdump objdump || true)}"
[[ -n "$CXX" && -n "$RC" && -n "$OBJDUMP" ]] || { echo 'Set CXX, RC and OBJDUMP to a trusted portable MinGW-w64 toolchain.' >&2; exit 1; }
command -v -- "$CMAKE" >/dev/null 2>&1 || { echo 'CMake required for shell wrapper; Windows build.ps1 works without CMake.' >&2; exit 1; }
args=(-S "$ROOT" -B "$BUILD_DIR" "-DCMAKE_BUILD_TYPE=$CONFIGURATION" "-DLOL_ARCHITECTURE=$ARCHITECTURE" "-DLOL_EXECUTABLE_NAME=${EXECUTABLE_NAME:-LOLBackup}" "-DCMAKE_CXX_COMPILER=$CXX" "-DCMAKE_RC_COMPILER=$RC" "-DCMAKE_OBJDUMP=$OBJDUMP")
case "$(uname -s)" in MINGW*|MSYS*|CYGWIN*) if command -v ninja >/dev/null 2>&1; then default_generator=Ninja; else default_generator="MinGW Makefiles"; fi; generator="${CMAKE_GENERATOR:-$default_generator}" ;; *) generator="${CMAKE_GENERATOR:-Unix Makefiles}"; args+=(-DCMAKE_SYSTEM_NAME=Windows) ;; esac
args+=(-G "$generator")
[[ -z "${MAKE:-}" ]] || args+=("-DCMAKE_MAKE_PROGRAM=$MAKE")
case "$MODE" in App) args+=(-DBUILD_TESTING=OFF) ;; Tests|All) args+=(-DBUILD_TESTING=ON) ;; *) echo 'MODE must be App, Tests or All' >&2; exit 1 ;; esac
"$CMAKE" "${args[@]}"
if [[ "$MODE" == Tests ]]; then
  targets=(test_sync test_sync_semantics test_core_bugs test_config_concurrency test_bridge_contract test_experience test_stress test_winver test_service_safety test_file_safety test_aux_safety test_tool_manager test_ui_geometry)
  "$CMAKE" --build "$BUILD_DIR" --config "$CONFIGURATION" --parallel "${JOBS:-2}" --target "${targets[@]}"
else
  "$CMAKE" --build "$BUILD_DIR" --config "$CONFIGURATION" --parallel "${JOBS:-2}"
fi
printf 'Build and dependency audits succeeded: %s\n' "$BUILD_DIR"
