#!/usr/bin/env bash
# usage: ci/fetch_upstream.sh <dest-dir>
# Checks out upstream KiCad at the commit in patches/UPSTREAM_BASE (shallow, by SHA) into
# <dest-dir> and applies KiCadD's patches. Used by CI; also handy for a clean local tree.
set -euo pipefail
here=$(cd "$(dirname "$0")/.." && pwd)
dest=$1
base=$(cut -d' ' -f1 "$here/patches/UPSTREAM_BASE")
url=${KICAD_UPSTREAM_URL:-https://gitlab.com/kicad/code/kicad.git}

git init -q "$dest"
git -C "$dest" config core.autocrlf false
git -C "$dest" fetch -q --depth 1 "$url" "$base"
git -C "$dest" checkout -q FETCH_HEAD
for p in "$here"/patches/*.patch; do
    git -C "$dest" apply --whitespace=nowarn "$p"
    echo "applied $(basename "$p")"
done
echo "upstream $(git -C "$dest" log -1 --format='%h %cs %s')"
