#!/usr/bin/env bash
# The console's set-up for testing, done from a computer over the network (FTP on port 1337: VitaShell's FTP server
# before vitacompanion is installed, vitacompanion's after; commands on port 1338: vitacompanion only). For an agent
# on the developer's computer: CLAUDE.md and docs/vita/testing.md say when to use which step.
#
#   port/vita/tools/vita_setup.sh check                       what is on the console (read only)
#   port/vita/tools/vita_setup.sh companion SUPRX SKPRX       vitacompanion into ur0:tai and its two lines into config.txt
#   port/vita/tools/vita_setup.sh install-dev [VPK]           the development package installed (vitacompanion's promote)
#   port/vita/tools/vita_setup.sh capunlocker on|off          CapUnlocker's line in config.txt (un)commented, then a reboot
#   port/vita/tools/vita_setup.sh reboot                      a reboot, and a wait for vitacompanion to answer again
#
#   env VITA_IP  the console's address (required)
#
# Every change of ur0:tai/config.txt first keeps the console's copy in port/build/vita_backup/config.<date>.txt (and the
# very first one as config.first.txt), changes
# only the lines it names, and is checked before it is sent. A config.txt that taiHEN cannot read stops the plugins
# from loading (vitacompanion among them): then the console needs a person (VitaShell, or the HENkaku guides' way to
# start without plugins), with the backup at hand.
# Needs: curl, nc (netcat), python3, unzip.
set -u
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
IP="${VITA_IP:-}"
[ -n "$IP" ] || { echo "vita_setup: VITA_IP is needed"; exit 2; }
FTP="ftp://$IP:1337"
BK="$ROOT/port/build/vita_backup"
mkdir -p "$BK"
cmd() { printf '%s\n' "$1" | nc -w 5 "$IP" 1338 2>/dev/null; }
get() { curl -s -f -o "$2" "$FTP/$1"; }                       # $1 like ux0:/data/bt3/bt3.txt
put() { curl -s -f --ftp-create-dirs -T "$1" "$FTP/$2"; }
lst() { curl -s -f -l "$FTP/$1"; }
companion_up() { [ -n "$(cmd version)" ]; }

check() {
  echo "== FTP (port 1337)"
  if lst "ux0:/" > "$BK/ls_ux0.txt"; then echo "  answers: ux0: has $(wc -l < "$BK/ls_ux0.txt") entries"; else echo "  NO ANSWER: start VitaShell and press SELECT (its FTP server), or install vitacompanion"; return 1; fi
  echo "== vitacompanion (port 1338)"
  v="$(cmd version)"; if [ -n "$v" ]; then echo "  answers: $(echo "$v" | head -2 | tr '\n' ' ')"; else echo "  not running (step: companion)"; fi
  echo "== ur0:tai/config.txt"
  if get "ur0:/tai/config.txt" "$BK/config.now.txt"; then
    grep -in "vitacompanion\|capunlocker" "$BK/config.now.txt" | sed 's/^/  /' || echo "  neither vitacompanion nor CapUnlocker named"
  else echo "  not read"; fi
  echo "== ur0:data/libshacccg.suprx (the shader compiler)"
  lst "ur0:/data/" 2>/dev/null | grep -qi "^libshacccg.suprx" && echo "  there" || echo "  NOT THERE: the game cannot compile its shaders (README: Installation)"
  echo "== the game"
  lst "ux0:/data/bt3/disc/" 2>/dev/null | grep -q "SLUS_216.78" && echo "  disc files: there" || echo "  disc files: NOT in ux0:data/bt3/disc"
  lst "ux0:/app/" > "$BK/ls_app.txt" 2>/dev/null || :
  for t in BTTD00010 BTTD00008; do grep -q "^$t" "$BK/ls_app.txt" && echo "  $t installed" || echo "  $t not installed"; done
  echo "  (BTTD00010 the release, BTTD00008 the development build)"
}

# config.txt changed by a python function over its lines, checked, sent; the old one kept
edit_config() {
  local how="$1" stamp; stamp="$(date +%Y%m%d-%H%M%S)-$$-$RANDOM"
  get "ur0:/tai/config.txt" "$BK/config.$stamp.txt" || { echo "vita_setup: ur0:tai/config.txt not read"; return 1; }
  [ -f "$BK/config.first.txt" ] || cp "$BK/config.$stamp.txt" "$BK/config.first.txt"   # (the console's own, before any change)
  python3 - "$BK/config.$stamp.txt" "$BK/config.new.txt" "$how" <<'EOF' || return 1
import sys
src, dst, how = sys.argv[1], sys.argv[2], sys.argv[3]
lines = open(src, encoding="utf-8", errors="replace").read().splitlines()
def section(name):
    for i, l in enumerate(lines):
        if l.strip().lower() == name.lower():
            return i
    return -1
def ensure(sec, path):
    if any(l.strip() == path for l in lines):
        return
    i = section(sec)
    if i < 0:
        if sec == "*KERNEL":
            lines[0:0] = [sec, path]
        else:
            lines.extend([sec, path])
    else:
        lines.insert(i + 1, path)
if how == "companion":
    ensure("*KERNEL", "ur0:tai/vitacompanion_kernel.skprx")
    ensure("*main", "ur0:tai/vitacompanion.suprx")
elif how in ("cap-on", "cap-off"):
    hits = [i for i, l in enumerate(lines) if "capunlocker" in l.lower()]
    if not hits:
        sys.exit("vita_setup: no CapUnlocker line in config.txt: it is not installed")
    for i in hits:
        bare = lines[i].lstrip("#").strip()
        lines[i] = bare if how == "cap-on" else "#" + bare
# the checks: the sections still there, every plugin line a path of a plugin
assert section("*KERNEL") >= 0, "no *KERNEL section"
for l in lines:
    s = l.strip()
    if s and not s.startswith("#") and not s.startswith("*"):
        assert ":" in s and s.lower().endswith((".skprx", ".suprx")), "not a plugin's path: " + s
open(dst, "w", encoding="utf-8", newline="\n").write("\n".join(lines) + "\n")
EOF
  echo "--- the change (kept: $BK/config.$stamp.txt)"
  diff "$BK/config.$stamp.txt" "$BK/config.new.txt" || true
  put "$BK/config.new.txt" "ur0:/tai/config.txt" || { echo "vita_setup: config.txt NOT sent"; return 1; }
  echo "vita_setup: config.txt sent"
}

reboot_wait() {
  companion_up || { echo "vita_setup: vitacompanion does not answer: reboot the console by hand"; return 1; }
  cmd reboot > /dev/null
  echo "vita_setup: rebooting; waiting for vitacompanion (up to 4 minutes)"
  sleep 30
  for _ in $(seq 1 42); do
    if companion_up; then echo "vita_setup: back"; return 0; fi
    sleep 5
  done
  echo "vita_setup: NO ANSWER after the reboot: a person must look at the console (the old config.txt is in $BK)"
  return 1
}

case "${1:-}" in
  check) check ;;
  companion)
    [ -f "${2:-}" ] && [ -f "${3:-}" ] || { echo "usage: vita_setup.sh companion vitacompanion.suprx vitacompanion_kernel.skprx"; exit 2; }
    put "$2" "ur0:/tai/vitacompanion.suprx" && put "$3" "ur0:/tai/vitacompanion_kernel.skprx" || { echo "vita_setup: the plugin's files not sent"; exit 3; }
    edit_config companion || exit 3
    echo "vita_setup: now the console must be restarted once by hand (vitacompanion is not running yet to do it)"
    ;;
  install-dev)
    VPK="${2:-$ROOT/port/build/vita_dev/Tenkaichi3Decomp-Vita-dev.vpk}"
    [ -f "$VPK" ] || { echo "vita_setup: $VPK not found (BT3_VITA_DEV=1 python3 port/vita/build.py)"; exit 2; }
    companion_up || { echo "vita_setup: vitacompanion does not answer (step: companion)"; exit 3; }
    TMP="$(mktemp -d)"; unzip -q "$VPK" -d "$TMP" || exit 3
    DEST="ux0:/data/bt3_devpkg"
    (cd "$TMP" && find . -type f | sed 's|^\./||') | while read -r f; do
      put "$TMP/$f" "$DEST/$f" || { echo "vita_setup: $f not sent"; exit 3; }
    done || exit 3
    rm -rf "$TMP"
    echo "vita_setup: promote: $(cmd "promote ux0:data/bt3_devpkg" | head -3 | tr '\n' ' ')"
    sleep 5
    lst "ux0:/app/" | grep -q "^BTTD00008" && echo "vita_setup: BTTD00008 installed" || { echo "vita_setup: BTTD00008 NOT installed (install the .vpk with VitaShell by hand)"; exit 3; }
    ;;
  capunlocker)
    case "${2:-}" in on) how=cap-on ;; off) how=cap-off ;; *) echo "usage: vita_setup.sh capunlocker on|off"; exit 2 ;; esac
    edit_config "$how" && reboot_wait
    ;;
  reboot) reboot_wait ;;
  *) sed -n '2,20p' "$0"; exit 2 ;;
esac
