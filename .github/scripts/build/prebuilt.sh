#!/usr/bin/env bash
# Immutable, hash-named build products stored as assets of the `prebuilt` release.
#   prebuilt.sh fetch <asset> <dir>    extract <asset> into <dir>; exit 3 when no such asset is published
#   prebuilt.sh publish <asset> <dir> <path>...   archive <path>... (relative to <dir>) and upload it once
# Names carry the full input hash, so an asset is never replaced and an older one is never substituted.
set -euo pipefail
release=prebuilt
repo=${GITHUB_REPOSITORY:-ZimengXiong/PancakeSlicer}
cmd=$1 asset=$2 dir=$3
shift 3

assets() {
  local out
  if out=$(gh release view "$release" --repo "$repo" --json assets --jq '.assets[].name' 2>&1); then
    printf '%s\n' "$out"
  elif grep -q 'release not found' <<<"$out"; then
    return 0
  else
    printf '%s\n' "$out" >&2
    return 1
  fi
}

case "$cmd" in
  fetch)
    names=$(assets)
    if ! grep -qxF "$asset" <<<"$names"; then echo "$asset is not published"; exit 3; fi
    tmp=$(mktemp -d)
    gh release download "$release" --repo "$repo" --pattern "$asset" --dir "$tmp"
    mkdir -p "$dir"
    tar -xzf "$tmp/$asset" -C "$dir"
    echo "Fetched $asset ($(du -h "$tmp/$asset" | cut -f1))"
    rm -rf "$tmp"
    ;;
  publish)
    tmp=$(mktemp -d)
    tar -czf "$tmp/$asset" -C "$dir" "$@"
    if ! gh release view "$release" --repo "$repo" >/dev/null 2>&1; then
      gh release create "$release" --repo "$repo" --prerelease --title 'Prebuilt CI build products' \
        --notes 'Native dependency prefixes and compiled engines keyed by the hash of their inputs. Written by CI; do not edit.' \
        || gh release view "$release" --repo "$repo" >/dev/null   # another job created it first
    fi
    if grep -qxF "$asset" <<<"$(assets)"; then
      echo "$asset is already published"
    elif ! gh release upload "$release" --repo "$repo" "$tmp/$asset"; then
      grep -qxF "$asset" <<<"$(assets)" || exit 1   # a concurrent job published the same inputs first
    fi
    echo "Published $asset ($(du -h "$tmp/$asset" | cut -f1))"
    rm -rf "$tmp"
    ;;
  *) echo "usage: prebuilt.sh fetch|publish <asset> <dir> [paths...]" >&2; exit 2 ;;
esac
