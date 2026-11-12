#!/usr/bin/env bash
# One test run of the development build on a PS Vita, driven from a computer on the same network through the
# vitacompanion plugin (FTP on port 1337, commands on 1338: https://github.com/devnoname120/vitacompanion).
# docs/vita/testing.md says how to set the console up once.
#
#   port/vita/tools/vita_console_run.sh --ip 192.168.1.50 --settings my_bt3.txt --seconds 600 [--eboot] [--out DIR]
#
#   --ip        the Vita's address (or VITA_IP)
#   --settings  a bt3.txt for this run, put in ux0:data/bt3/bt3.txt (the console's own is saved first and put back)
#   --seconds   how long the game runs before it is closed (default 300)
#   --eboot     first copy port/build/vita_dev/eboot.bin over the installed one (the package must have been installed
#               once with VitaShell: this replaces only the program)
#   --title     the title id (default BTTD00008, the development build's)
#   --out       where the run's files go (default port/build/runs/<date-time>)
#
# Leaves in --out: log.txt (this run's part of ux0:data/bt3/log-vgl.txt), hang.txt if the watchdog wrote one, the
# list of crash dumps in ux0:data, and perfsum.txt (port/vita/tools/perfsum.py over the log) when the run had battles.
# Needs: curl, nc (netcat), python3.
set -u
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
IP="${VITA_IP:-}"; SETTINGS=""; SECONDS_RUN=300; EBOOT=0; TITLE=BTTD00008; OUT=""
while [ $# -gt 0 ]; do
  case "$1" in
    --ip) IP="$2"; shift 2 ;;
    --settings) SETTINGS="$2"; shift 2 ;;
    --seconds) SECONDS_RUN="$2"; shift 2 ;;
    --eboot) EBOOT=1; shift ;;
    --title) TITLE="$2"; shift 2 ;;
    --out) OUT="$2"; shift 2 ;;
    *) echo "vita_console_run: unknown argument $1"; exit 2 ;;
  esac
done
[ -n "$IP" ] || { echo "vita_console_run: --ip (or VITA_IP) is needed"; exit 2; }
[ -z "$SETTINGS" ] || [ -f "$SETTINGS" ] || { echo "vita_console_run: $SETTINGS not found"; exit 2; }
OUT="${OUT:-$ROOT/port/build/runs/$(date +%Y%m%d-%H%M%S)}"
mkdir -p "$OUT"
FTP="ftp://$IP:1337"
cmd() { printf '%s\n' "$1" | nc -w 5 "$IP" 1338; }
get() { curl -s --ftp-method nocwd -o "$2" "$FTP//$1"; }       # (the double slash: the Vita's path as it is)
put() { curl -s --ftp-method nocwd -T "$1" "$FTP//$2"; }

cmd version > "$OUT/companion.txt" 2>&1 || true
[ -s "$OUT/companion.txt" ] || { echo "vita_console_run: no answer from vitacompanion at $IP:1338"; exit 3; }
cmd "quit $TITLE" > /dev/null 2>&1 || true
if [ "$EBOOT" = 1 ]; then
  EB="$ROOT/port/build/vita_dev/eboot.bin"
  [ -f "$EB" ] || { echo "vita_console_run: $EB not found (BT3_VITA_DEV=1 python3 port/vita/build.py)"; exit 2; }
  put "$EB" "ux0:/app/$TITLE/eboot.bin" || { echo "vita_console_run: eboot.bin not sent"; exit 3; }
fi
SAVED=""
if [ -n "$SETTINGS" ]; then
  if get "ux0:/data/bt3/bt3.txt" "$OUT/bt3.console.txt" && [ -s "$OUT/bt3.console.txt" ]; then SAVED=1; fi
  put "$SETTINGS" "ux0:/data/bt3/bt3.txt" || { echo "vita_console_run: settings not sent"; exit 3; }
  cp "$SETTINGS" "$OUT/bt3.run.txt"
fi
echo "vita_console_run: $TITLE runs for $SECONDS_RUN s"
cmd "screen on; launch $TITLE" > /dev/null
sleep "$SECONDS_RUN"
cmd "quit $TITLE" > /dev/null
sleep 3
get "ux0:/data/bt3/log-vgl.txt" "$OUT/log-all.txt" || true
get "ux0:/data/bt3/hang-vgl.txt" "$OUT/hang.txt" || rm -f "$OUT/hang.txt"
curl -s --ftp-method nocwd -l "$FTP//ux0:/data/" | grep -i "psp2core\|psp2dmp" > "$OUT/crash_dumps.txt" || true
if [ "$SAVED" = 1 ]; then
  put "$OUT/bt3.console.txt" "ux0:/data/bt3/bt3.txt" || echo "vita_console_run: WARNING: the console's bt3.txt not put back (it is in $OUT)"
fi
if [ -s "$OUT/log-all.txt" ]; then   # this run's part: from the last start line on (the log is kept across runs)
  python3 - "$OUT/log-all.txt" "$OUT/log.txt" <<'EOF'
import sys
lines = open(sys.argv[1], encoding="utf-8", errors="replace").read().splitlines(True)
start = max([i for i, l in enumerate(lines) if l.startswith("vita: Tenkaichi3Decomp")] or [0])
open(sys.argv[2], "w", encoding="utf-8").writelines(lines[start:])
EOF
  if grep -q "^soak: battle" "$OUT/log.txt"; then
    python3 "$ROOT/port/vita/tools/perfsum.py" "$OUT/log.txt" > "$OUT/perfsum.txt" 2>&1 || true
    grep -q "side B" "$OUT/log.txt" && python3 "$ROOT/port/vita/tools/perfsum.py" --ab "$OUT/log.txt" > "$OUT/perfsum_ab.txt" 2>&1 || true
  fi
else
  echo "vita_console_run: no log came back"
fi
[ -s "$OUT/crash_dumps.txt" ] && echo "vita_console_run: crash dumps in ux0:data: $(tr '\n' ' ' < "$OUT/crash_dumps.txt")"
echo "vita_console_run: files in $OUT"
ls "$OUT"
