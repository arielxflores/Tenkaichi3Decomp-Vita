#!/bin/sh
# NauTaro's BT4decomp (https://github.com/NauTaro/DBZ-BT4-Tag-Team-Decomp) at the commit the Vita's changes were made
# for, with them applied (bt4decomp-vita.patch: the port's changes to the game code merged in, the online item of the
# main menu left out). Then: BT3_BT4=<folder> python3 port/vita/build.py  (see docs/vita/building.md)
#
#     port/vita/bt4/get_bt4decomp.sh [folder]        (default: port/build/bt4decomp)
set -e
here="$(cd "$(dirname "$0")" && pwd)"
dir="${1:-$here/../../build/bt4decomp}"
commit=92b87f49d152457fd8a88daa3a26551b22941f44
if [ ! -d "$dir/.git" ]; then
    mkdir -p "$dir"
    git -C "$dir" init -q
    git -C "$dir" remote add origin https://github.com/NauTaro/DBZ-BT4-Tag-Team-Decomp
fi
git -C "$dir" fetch -q --depth 1 origin "$commit"
git -C "$dir" checkout -q -f "$commit"
git -C "$dir" apply --whitespace=nowarn "$here/bt4decomp-vita.patch"
echo "BT4decomp $commit with the Vita's changes: $dir"
echo "build: BT3_BT4=$dir python3 port/vita/build.py"
