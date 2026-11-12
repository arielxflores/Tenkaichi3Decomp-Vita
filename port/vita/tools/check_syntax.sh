#!/bin/sh
# A check of the Vita build's C without VitaSDK: each file compiled by the host's clang for the Vita's processor
# (armv7-a, NEON) up to its syntax and types only, against the SDK's headers, vitaGL's with this port's patch and the
# samples' debugScreen.h, all cloned once into port/build/syntax_sdk (no toolchain, no linking, nothing run).
#
#     port/vita/tools/check_syntax.sh                     the renderer's files and the game files the port changes
#     port/vita/tools/check_syntax.sh port/vita/vita_vgl.c src/sys/vu1_packet.c ...
#     WARN=1 port/vita/tools/check_syntax.sh FILE ...     with the warnings (-Wall -Wextra) instead of errors only
#
# It finds what a compiler finds: undeclared names, wrong types, a missing declaration. It cannot find what only the
# console shows: a Cg shader the shader compiler refuses (they are compiled at run time), a GXM fault, a wrong picture.
# Needs: clang, git, network access to github.com the first time. Run from the repository root.
set -e
ROOT=$(pwd)
SDK=${BT3_SYNTAX_SDK:-$ROOT/port/build/syntax_sdk}
VITAGL_REV=c9bcd74 # (port/vita/vitagl/build_vitagl.sh)
mkdir -p "$SDK"
if [ ! -d "$SDK/vita-headers" ]; then git clone -q --depth 1 https://github.com/vitasdk/vita-headers "$SDK/vita-headers"; fi
if [ ! -d "$SDK/vitaShaRK" ]; then git clone -q --depth 1 https://github.com/Rinnegatamante/vitaShaRK "$SDK/vitaShaRK"; fi
if [ ! -d "$SDK/samples" ]; then git clone -q --depth 1 https://github.com/vitasdk/samples "$SDK/samples"; fi
if [ ! -d "$SDK/vitaGL" ]; then
    git clone -q https://github.com/Rinnegatamante/vitaGL "$SDK/vitaGL"
    (cd "$SDK/vitaGL" && git checkout -q "$VITAGL_REV" && git apply "$ROOT/port/vita/vitagl/vitagl.patch")
fi
# (the host's C library headers stand in for newlib's: a 32-bit glibc stub they ask for)
mkdir -p "$SDK/fake/gnu"
[ -f "$SDK/fake/gnu/stubs-32.h" ] || : > "$SDK/fake/gnu/stubs-32.h"

if [ $# -eq 0 ]; then
    set -- port/vita/vita_vgl.c port/vita/vita_main.c port/vita/vita_gfx.c port/src/gs/gs_core.c port/src/gs/gs_vu1.c \
        port/src/gs_marker.c src/battle/stgm_a.c src/sys/vu1_packet.c src/sys/gfxm_e_b.c src/sys/gfxm_d_c.c
fi
if [ -n "$WARN" ]; then
    DIAG="-Wall -Wextra -Wno-unused-parameter -Wno-sign-compare"
else
    DIAG="-Wno-everything -Werror=implicit-function-declaration -Werror=incompatible-pointer-types -Werror=int-conversion"
fi
fail=0
for f in "$@"; do
    # (the flags of port/vita/build.py's COMMON; -D__regparm__: an x86 attribute of the host's pthread.h)
    out=$(clang --target=armv7a-none-eabi -mfloat-abi=hard -mcpu=cortex-a9 -mfpu=neon -fsyntax-only -std=gnu99 $DIAG \
        -DPORT_VITA=1 -DPORT_VITA_FIXED=1 '-DPORT_VITA_SFX="-vgl.txt"' -DPORT_VITA_GFX=1 -DPORT_VITA_VGL=1 \
        -DMPEG2_PLAYER=1 -DPORT_VITA_SOUND=1 -DPORT_VITA_NATIVE=1 '-D__regparm__(x)=' \
        -Iinclude -Iport/include -Iport/src -Iport/vita -Iport/src/gs -Iport/vita/sdlshim -include port/vita/vita_cold.h \
        -I"$SDK/vita-headers/include" -I"$SDK/vitaGL/source" -I"$SDK/vitaShaRK/source" -I"$SDK/samples/common" \
        -I"$SDK/fake" -isystem /usr/include -isystem /usr/include/x86_64-linux-gnu "$f" 2>&1) || fail=1
    if [ -n "$out" ]; then
        echo "== $f"
        echo "$out" | grep -E "error|warning" | head -40
    else
        echo "== $f: clean"
    fi
done
exit $fail
