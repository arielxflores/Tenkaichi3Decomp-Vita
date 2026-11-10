#!/bin/bash
# Adds up the profile counts of runs of a BT3_PGO=gen build (port/vita/vita_pgo.c) into port/vita/pgo/<rel|dev>/, for
# a BT3_PGO=use build:
#
#     port/vita/pgo_merge.sh rel <folder> [<folder> ...]
#
# Each <folder> is a copy of one of the Vita's ux0:data/bt3/pgo/run-<n>/ (a run of the gen build: its .gcda files,
# the counts of the whole run). The counts of all of them, and of what port/vita/pgo/<rel|dev>/ holds already, are
# added (gcov-tool merge), so profiles of several kinds of play (menus, fights on different stages, story mode) can be
# gathered over time. To start over, delete port/vita/pgo/<rel|dev>/.
set -e
here="$(cd "$(dirname "$0")" && pwd)"
kind="$1"; shift || true
if [ "$kind" != rel ] && [ "$kind" != dev ] || [ $# -eq 0 ]; then
    echo "usage: $0 rel|dev <folder of .gcda files> ..." >&2
    exit 2
fi
tool="${VITASDK:-/usr/local/vitasdk}/bin/arm-vita-eabi-gcov-tool"
out="$here/pgo/$kind"
mkdir -p "$out"
for d in "$@"; do
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
