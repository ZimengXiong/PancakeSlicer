#!/usr/bin/env bash
# Configure the headless engine against engine/deps/build and, unless --configure-only, build
# nonplanar_slice_tool into build-native/tool.
#   engine.sh <os> <arch> [--configure-only]     the configure step lives in engine-<os>.sh
set -euo pipefail
os=$1 arch=$2 mode=${3:-}
common=(-DCMAKE_BUILD_TYPE=Release -DSLIC3R_GUI=OFF -DSLIC3R_CAD=OFF -DBUILD_TESTS=ON -DDEP_BUILD_DIR="$PWD/engine/deps/build")
ccache=(-DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache)
source "$(dirname "$0")/engine-$os.sh"
configure "$arch"
[ "$mode" = --configure-only ] && exit 0
if command -v ccache >/dev/null; then ccache --max-size=1500M; fi
cmake --build build-native --config Release --target nonplanar_slice_tool --parallel 4
mkdir -p build-native/tool
# macOS links the tool as an .app bundle; take the executable itself.
find build-native/tests/fff_print -path '*Release*' -type f \( -name nonplanar_slice_tool -o -name nonplanar_slice_tool.exe \) \
  -exec cp {} build-native/tool/ \;
ls -la build-native/tool
[ -n "$(ls build-native/tool)" ]
