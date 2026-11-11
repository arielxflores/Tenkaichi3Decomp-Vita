#!/bin/bash
# Adds up the profile counts of runs of a BT3_PGO=gen build (port/vita/vita_pgo.c) into port/vita/pgo/<rel|dev>/, for
# a BT3_PGO=use build:
#
#     port/vita/pgo_merge.sh rel <run.pgo or folder> [...]
#
# Each <run.pgo> is one of the Vita's ux0:data/bt3/pgo/*.pgo (a run of the gen build: the .gcda files of all its
# objects in one file, the counts of the whole run); a <folder> holds .gcda files. The counts of all of them, and of
# what port/vita/pgo/<rel|dev>/ holds already, are added (gcov-tool merge), so profiles of several kinds of play
# (menus, fights on different stages, story mode) can be gathered over time. To start over, delete
# port/vita/pgo/<rel|dev>/. The counts of a development build serve a release build too (the game's code is the same;
# a function that differs is built without its counts, with a warning).
set -e
here="$(cd "$(dirname "$0")" && pwd)"
kind="$1"; shift || true
if [ "$kind" != rel ] && [ "$kind" != dev ] || [ $# -eq 0 ]; then
    echo "usage: $0 rel|dev <run.pgo or folder of .gcda files> ..." >&2
    exit 2
fi
unpacked="$(mktemp -d)"
trap 'rm -rf "$unpacked"' EXIT
unpack() { # a .pgo file's .gcda files into a folder of their own (vita_pgo.c: "BT3PGO1\n", then length + name, length + bytes)
    python3 - "$1" "$2" <<'PY'
import os, struct, sys
data = open(sys.argv[1], 'rb').read()
if data[:8] != b'BT3PGO1\n':
    sys.exit(sys.argv[1] + ': not a profile file of vita_pgo.c')
os.makedirs(sys.argv[2], exist_ok=True)
at, n = 8, 0
while at + 4 <= len(data):
    (k,) = struct.unpack_from('<I', data, at); name = data[at + 4:at + 4 + k].decode(); at += 4 + k
    (m,) = struct.unpack_from('<I', data, at); body = data[at + 4:at + 4 + m]; at += 4 + m
    if len(body) != m or '/' in name or not name.endswith('.gcda'):
        sys.exit(sys.argv[1] + ': cut short or damaged')
    open(os.path.join(sys.argv[2], name), 'wb').write(body); n += 1
print('%s: %d objects' % (sys.argv[1], n))
PY
}
tool="${VITASDK:-/usr/local/vitasdk}/bin/arm-vita-eabi-gcov-tool"
out="$here/pgo/$kind"
mkdir -p "$out"
i=0
for d in "$@"; do
    if [ -f "$d" ]; then
        i=$((i + 1))
        unpack "$d" "$unpacked/$i" || exit 1
        d="$unpacked/$i"
    fi
    n=$(ls "$d"/*.gcda 2>/dev/null | wc -l)
    if [ "$n" -eq 0 ]; then
        echo "$d: no .gcda files" >&2
        exit 1
    fi
    if [ -z "$(ls "$out"/*.gcda 2>/dev/null)" ]; then
        cp "$d"/*.gcda "$out"/
    else
        tmp="$(mktemp -d)"
        "$tool" merge -o "$tmp" "$out" "$d"
        cp "$tmp"/*.gcda "$out"/
        rm -rf "$tmp"
    fi
    echo "$d: $n files added"
done
echo "$(ls "$out"/*.gcda | wc -l) files in $out"
