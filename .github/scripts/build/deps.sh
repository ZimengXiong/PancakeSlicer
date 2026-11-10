#!/usr/bin/env bash
# Build the headless engine's native dependencies into engine/deps/build/OrcaSlicer_dep.
#   deps.sh <os> <arch>     the configure step lives in deps-<os>.sh so each OS's prefix is keyed on its own recipe
# The deps target also builds wxWidgets and wxInspector, which only the desktop GUI uses: wxWidgets needs GTK on
# Linux and its macOS patch no longer applies. Build the listed dependencies that exist on this platform.
set -euo pipefail
os=$1 arch=$2
here=$(dirname "$0")
source "$here/deps-$os.sh"
configure "$arch"
targets=()
for t in $(cat "$here/deps-targets.txt"); do
  if has "$t"; then targets+=("$t"); fi
done
echo "Building: ${targets[*]}"
cmake --build engine/deps/build --config Release --target "${targets[@]}" --parallel 4
