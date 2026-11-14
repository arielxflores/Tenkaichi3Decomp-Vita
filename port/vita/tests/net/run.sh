#!/bin/bash
# The multiplayer code (port/vita/vita_net.c) run on a PC: two copies of t_net are two Vitas, their ad hoc line is UDP
# on this machine. Each finds the other as the versus menu does (or, with "start", as the start menu does), plays
# matches whose "game" folds the buttons it is given into a number, and writes that number once a second of blanks.
# The two records must be the same.
#
#     port/vita/tests/net/run.sh [matches] [packets lost, percent] [menu | start | mixed]      (default: 3 10 menu)
#
# Needs gcc and VitaSDK's headers ($VITASDK): the system's calls are stand-ins (t_net.c, stubs.c).
set -e
here="$(cd "$(dirname "$0")" && pwd)"
out="${TMPDIR:-/tmp}/bt3-nettest.$$"
mkdir -p "$out/inc"
for d in psp2 psp2common vitasdk; do ln -sfn "$VITASDK/arm-vita-eabi/include/$d" "$out/inc/$d"; done
gcc -O1 -g -w -I"$out/inc" -I"$here/../.." -I"$here/../../../../include" -I"$here/../../../include" "$here/t_net.c" "$here/stubs.c" -o "$out/t_net"
n="${1:-3}"; loss="${2:-10}"; a=1; b=2
case "${3:-menu}" in start) a=11; b=12; n=1 ;; mixed) b=12; n=1 ;; esac
timeout 300 "$out/t_net" $a "$n" "$loss" "$out/1.txt" 2> "$out/1.log" &
timeout 300 "$out/t_net" $b "$n" "$loss" "$out/2.txt" 2> "$out/2.log" || true
wait || true
if cmp -s <(grep -v '^done' "$out/1.txt") <(grep -v '^done' "$out/2.txt") && [ "$(grep -c '^[0-9]' "$out/1.txt")" -gt 20 ]; then
    echo "the same on both: $(grep -c '^[0-9]' "$out/1.txt") records in $(grep -c '^match' "$out/1.txt") match(es); $(grep -h 'comparisons' "$out/1.log" | tail -1 | sed 's/.*(\(.*\))/\1/')"
    rm -rf "$out"
else
    echo "NOT the same (or no match was played): see $out"
    exit 1
fi
