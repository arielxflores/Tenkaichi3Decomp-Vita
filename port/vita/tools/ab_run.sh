#!/usr/bin/env bash
# Two test runs of the development build, A with a setting at 0 (the way before) and B at 1 (the new way), in Vita3K
# or on the console, then their logs set side by side by runcmp.py. docs/vita/compare.md is the workflow.
#
#   port/vita/tools/ab_run.sh --switch native [--where vita3k|console] [--base FILE] [--seconds N] [--a 0] [--b 1]
#
#   --switch   the bt3.txt setting under test (native, fxlow, gxmcache, ...)
#   --where    vita3k (default; vita3k_run.sh, env VITA3K / VITA3K_PREF) or console (vita_console_run.sh, env VITA_IP;
#              sends port/build/vita_dev/eboot.bin first)
#   --base     the settings both runs share (default port/vita/tools/settings/fight_ab.txt: one fixed fight, drawlog)
#   --seconds  each run's length (default 240)
#   --a, --b   the setting's two values (default 0 and 1)
#   --vpk      (vita3k) the package, if not port/build/vita_dev/Tenkaichi3Decomp-Vita-dev.vpk
#
# Leaves port/build/runs/ab-<switch>-<date>/{A,B}/ (each run's files) and report.txt (runcmp.py's comparison); the
# verdict is the report's last line, and the exit status 0 only for OK.
set -u
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
SWITCH=""; VPK=""; WHERE=vita3k; BASE="$ROOT/port/vita/tools/settings/fight_ab.txt"; SECS=240; VA=0; VB=1
while [ $# -gt 0 ]; do
  case "$1" in
    --switch) SWITCH="$2"; shift 2 ;;
    --where) WHERE="$2"; shift 2 ;;
    --base) BASE="$2"; shift 2 ;;
    --seconds) SECS="$2"; shift 2 ;;
    --a) VA="$2"; shift 2 ;;
    --b) VB="$2"; shift 2 ;;
    --vpk) VPK="$2"; shift 2 ;;
    *) echo "ab_run: unknown argument $1"; exit 2 ;;
  esac
done
[ -n "$SWITCH" ] || { echo "ab_run: --switch is needed"; exit 2; }
[ -f "$BASE" ] || { echo "ab_run: $BASE not found"; exit 2; }
DIR="$ROOT/port/build/runs/ab-$SWITCH-$(date +%Y%m%d-%H%M%S)"
mkdir -p "$DIR"
first=1
for side in A B; do
  val=$VA; [ $side = B ] && val=$VB
  { cat "$BASE"; echo; echo "$SWITCH=$val"; } > "$DIR/settings_$side.txt"   # (the last line of a setting wins)
  echo "ab_run: side $side, $SWITCH=$val, $WHERE, $SECS s"
  if [ "$WHERE" = console ]; then
    extra=(); [ $first = 1 ] && extra=(--eboot)
    "$ROOT/port/vita/tools/vita_console_run.sh" --settings "$DIR/settings_$side.txt" --seconds "$SECS" --out "$DIR/$side" "${extra[@]}"
  else
    extra=(); [ -n "$VPK" ] && extra=(--vpk "$VPK")
    "$ROOT/port/vita/tools/vita3k_run.sh" --settings "$DIR/settings_$side.txt" --seconds "$SECS" --out "$DIR/$side" "${extra[@]}"
  fi
  first=0
done
if [ -s "$DIR/A/log.txt" ] && [ -s "$DIR/B/log.txt" ]; then
  python3 "$ROOT/port/vita/tools/runcmp.py" "$DIR/A/log.txt" "$DIR/B/log.txt" > "$DIR/report.txt"
  rc=$?
  cat "$DIR/report.txt"
  echo "ab_run: $DIR"
  exit $rc
fi
echo "ab_run: a run gave no log; see $DIR"
exit 3
