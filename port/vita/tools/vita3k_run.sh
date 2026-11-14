#!/usr/bin/env bash
# One test run of the development build in the Vita3K emulator (https://vita3k.org), bounded in time, after
# TwinsanityEvolved's tools/vita/vita3k_run.sh: the package installed and run by Vita3K's plain SDL window in console
# mode (--frontend sdl -z), stopped after --seconds, or at once if Vita3K's own log runs away (an emulated fault it
# retries fills the disk in minutes). docs/vita/testing.md says how to set Vita3K up once and what it can and cannot
# show (not the console's speed, nor the fourth core).
#
#   port/vita/tools/vita3k_run.sh --settings my_bt3.txt --seconds 300 [--vpk PATH] [--out DIR]
#
#   --vpk       default port/build/vita_dev/Tenkaichi3Decomp-Vita-dev.vpk
#   --settings  a bt3.txt for this run (Vita3K's own ux0:data/bt3/bt3.txt is saved first and put back)
#   --seconds   default 300
#   --out       default port/build/runs/vita3k-<date-time>
#   env VITA3K       the emulator's program (default: vita3k / Vita3K on the PATH, or the macOS application)
#       VITA3K_PREF  Vita3K's folder of the emulated memory card and its log (default: Linux
#                    ${XDG_DATA_HOME:-~/.local/share}/Vita3K/Vita3K, macOS ~/Library/Application Support/Vita3K/Vita3K;
#                    Vita3K's settings name it "Emulated System Storage Folder")
#
# Leaves in --out: log.txt (this run's part of ux0:data/bt3/log-vgl.txt), hang.txt if any, vita3k.log's tail,
# perfsum.txt when the run had battles.
set -u
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
VPK="$ROOT/port/build/vita_dev/Tenkaichi3Decomp-Vita-dev.vpk"; SETTINGS=""; SECONDS_MAX=300; OUT=""; MAX_LOG_MB=50
while [ $# -gt 0 ]; do
  case "$1" in
    --vpk) VPK="$2"; shift 2 ;;
    --settings) SETTINGS="$2"; shift 2 ;;
    --seconds) SECONDS_MAX="$2"; shift 2 ;;
    --out) OUT="$2"; shift 2 ;;
    *) echo "vita3k_run: unknown argument $1"; exit 2 ;;
  esac
done
if [ -z "${VITA3K:-}" ]; then
  VITA3K="$(command -v vita3k || command -v Vita3K || echo /Applications/Vita3K.app/Contents/MacOS/Vita3K)"
fi
if [ -z "${VITA3K_PREF:-}" ]; then
  if [ "$(uname)" = Darwin ]; then VITA3K_PREF="$HOME/Library/Application Support/Vita3K/Vita3K"
  else VITA3K_PREF="${XDG_DATA_HOME:-$HOME/.local/share}/Vita3K/Vita3K"; fi
fi
FS="$VITA3K_PREF/fs"
[ -x "$VITA3K" ] || { echo "vita3k_run: $VITA3K not found (set VITA3K)"; exit 2; }
[ -f "$VPK" ] || { echo "vita3k_run: $VPK not found (BT3_VITA_DEV=1 python3 port/vita/build.py)"; exit 2; }
[ -d "$FS/ux0" ] || { echo "vita3k_run: $FS/ux0 not found (set VITA3K_PREF; install the firmware in Vita3K once)"; exit 2; }
[ -f "$FS/ux0/data/bt3/disc/SLUS_216.78" ] || { echo "vita3k_run: the game files are not in $FS/ux0/data/bt3/disc"; exit 2; }
[ -f "$FS/ur0/data/libshacccg.suprx" ] || echo "vita3k_run: WARNING: no $FS/ur0/data/libshacccg.suprx: shaders not in ux0:data/bt3/shcache cannot be compiled"
[ -z "$SETTINGS" ] || [ -f "$SETTINGS" ] || { echo "vita3k_run: $SETTINGS not found"; exit 2; }
if pgrep -x Vita3K > /dev/null || pgrep -x vita3k > /dev/null; then echo "vita3k_run: Vita3K is running already; one at a time"; exit 3; fi
OUT="${OUT:-$ROOT/port/build/runs/vita3k-$(date +%Y%m%d-%H%M%S)}"
mkdir -p "$OUT"
BT3="$FS/ux0/data/bt3"
SAVED=""
if [ -n "$SETTINGS" ]; then
  [ -f "$BT3/bt3.txt" ] && cp "$BT3/bt3.txt" "$OUT/bt3.vita3k.txt" && SAVED=1
  cp "$SETTINGS" "$BT3/bt3.txt"
  cp "$SETTINGS" "$OUT/bt3.run.txt"
fi
: > "$VITA3K_PREF/vita3k.log" 2>/dev/null || true
rm -f "$BT3/hang-vgl.txt"
"$VITA3K" --frontend sdl -z --log-level 4 "$VPK" > "$OUT/vita3k.stdout.txt" 2>&1 &
PID=$!
start=$(date +%s); reason="exited"
while kill -0 "$PID" 2>/dev/null; do
  if [ $(( $(date +%s) - start )) -ge "$SECONDS_MAX" ]; then reason="stopped after ${SECONDS_MAX} s"; kill "$PID"; sleep 2; kill -9 "$PID" 2>/dev/null; break; fi
  size=$(wc -c < "$VITA3K_PREF/vita3k.log" 2>/dev/null || echo 0)
  if [ $(( size / 1048576 )) -ge "$MAX_LOG_MB" ]; then reason="vita3k.log over ${MAX_LOG_MB} MB (an emulated fault in a loop): killed"; kill -9 "$PID"; break; fi
  sleep 1
done
wait "$PID" 2>/dev/null
echo "vita3k_run: $reason after $(( $(date +%s) - start )) s"
tail -40 "$VITA3K_PREF/vita3k.log" > "$OUT/vita3k.log.tail.txt" 2>/dev/null || true
[ -f "$BT3/hang-vgl.txt" ] && cp "$BT3/hang-vgl.txt" "$OUT/hang.txt"
[ "$SAVED" = 1 ] && cp "$OUT/bt3.vita3k.txt" "$BT3/bt3.txt"
if [ -s "$BT3/log-vgl.txt" ]; then   # this run's part: from the last start line on (the log is kept across runs)
  python3 - "$BT3/log-vgl.txt" "$OUT/log.txt" <<'EOF'
import sys
lines = open(sys.argv[1], encoding="utf-8", errors="replace").read().splitlines(True)
start = max([i for i, l in enumerate(lines) if l.startswith("vita: Tenkaichi3Decomp")] or [0])
open(sys.argv[2], "w", encoding="utf-8").writelines(lines[start:])
EOF
  grep -q "^soak: battle" "$OUT/log.txt" && python3 "$ROOT/port/vita/tools/perfsum.py" "$OUT/log.txt" > "$OUT/perfsum.txt" 2>&1
else
  echo "vita3k_run: no log in $BT3 (see $OUT/vita3k.log.tail.txt)"
fi
echo "vita3k_run: files in $OUT"
ls "$OUT"
