#!/usr/bin/env bash
# Print the prebuilt asset names for one target to $GITHUB_OUTPUT (or stdout):
#   deps=deps-<target>-<hash>      native dependency prefix: engine/deps, this OS's deps recipe, compiler
#   engine=engine-<target>-<hash>  compiled nonplanar_slice_tool: the deps key plus every engine input
#   source=<sha1>                  engine source identity compiled into the tool and bound by qualification
#   reference=engine-reference-<hash>  Linux reference qualification for that source, resources and qualifier
#     (packaging, such as desktop/scripts/stage-ci.py, is left out so a packaging change reuses the reference)
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
code_files() { git ls-files -s -- "$@" | awk -F '\t' '$2 !~ /\.md$/'; }
code_tree() { code_files "$@" | h; }
deps_hash=$(printf '%s\n%s\n%s\n' "$target" "$compiler" "$(tree engine/deps "$b/deps.sh" "$b/deps-targets.txt" "$b/deps-$os.sh")" | h)
code=(engine/src engine/deps_src engine/tests/fff_print engine/cmake engine/CMakeLists.txt)
source_sha=$(code_files "${code[@]}" | if command -v sha1sum >/dev/null; then sha1sum; else shasum -a 1; fi | cut -c1-40)
engine_hash=$(printf '%s\n%s\n' "$deps_hash" "$(code_tree "${code[@]}" "$b/engine.sh" "$b/engine-$os.sh")" | h)
reference_hash=$(printf '%s\n%s\n' "$source_sha" "$(code_tree engine/resources engine/scripts/qualification)" | h)
echo "Compiler: $compiler" >&2
{
  echo "deps=deps-$target-$deps_hash.tar.gz"
  echo "engine=engine-$target-$engine_hash.tar.gz"
  echo "source=$source_sha"
  echo "reference=engine-reference-$reference_hash.tar.gz"
} >> "${GITHUB_OUTPUT:-/dev/stdout}"
