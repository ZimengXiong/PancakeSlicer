#!/usr/bin/env bash
# Print the prebuilt asset names for one target to $GITHUB_OUTPUT (or stdout):
#   deps=deps-<target>-<hash>      native dependency prefix: engine/deps, this OS's deps recipe, compiler
#   engine=engine-<target>-<hash>  compiled nonplanar_slice_tool: the deps key plus every engine input
# Hashes come from git blob ids, so they are identical on every OS and line-ending setting.
#   keys.sh <target> <os>
set -euo pipefail
target=$1 os=$2
cd "$(git rev-parse --show-toplevel)"
case "$os" in
  linux) compiler=$(gcc --version | head -1) ;;
  mac) compiler=$(clang --version | head -1) ;;
  win) compiler=$(cl 2>&1 | head -1 | tr -d '\r') ;;
esac
b=.github/scripts/build
h() { if command -v sha256sum >/dev/null; then sha256sum; else shasum -a 256; fi | cut -c1-16; }   # macOS has only shasum
tree() { git ls-files -s -- "$@" | h; }
deps_hash=$(printf '%s\n%s\n%s\n' "$target" "$compiler" "$(tree engine/deps "$b/deps.sh" "$b/deps-targets.txt" "$b/deps-$os.sh")" | h)
engine_hash=$(printf '%s\n%s\n' "$deps_hash" "$(tree engine/src engine/deps_src engine/tests/fff_print engine/cmake engine/CMakeLists.txt \
  engine/scripts/engine-source.json "$b/engine.sh" "$b/engine-$os.sh")" | h)
echo "Compiler: $compiler" >&2
{
  echo "deps=deps-$target-$deps_hash.tar.gz"
  echo "engine=engine-$target-$engine_hash.tar.gz"
} >> "${GITHUB_OUTPUT:-/dev/stdout}"
