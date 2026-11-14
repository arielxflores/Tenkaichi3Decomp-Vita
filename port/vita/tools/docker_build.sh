#!/bin/sh
# The Vita build on a computer without Linux (macOS; Windows without WSL): port/vita/build.py inside a Docker image
# with VitaSDK, its packages and vitaGL with this port's patch (port/vita/docker/Dockerfile), built once.
#
#     port/vita/tools/docker_build.sh                     the release (port/build/vita_rel)
#     BT3_VITA_DEV=1 port/vita/tools/docker_build.sh      the development build (port/build/vita_dev)
#     port/vita/tools/docker_build.sh compile|link|vpk    one step, as build.py
#     port/vita/tools/docker_build.sh --image             only (re)build the image
#
# The image is amd64 (VitaSDK's toolchain); on an ARM Mac it runs under Docker's emulation, slower but the same.
# The variables of build.py (BT3_VITA_DEV, BT3_INCREMENTAL, BT3_VITA_TITLE_ID, BT3_VITA_TITLE, BT3_VITA_VPK) are
# passed through. Run from the repository root.
set -e
IMAGE=bt3-vita-build:c9bcd74
ROOT=$(pwd)
[ -f port/vita/build.py ] || { echo "run from the repository root"; exit 1; }
CTX=$(mktemp -d)
trap 'rm -rf "$CTX"' EXIT
cp port/vita/docker/Dockerfile port/vita/vitagl/vitagl.patch port/vita/vitagl/build_vitagl.sh "$CTX/"
# (the image's tag carries vitaGL's commit; a changed patch rebuilds its last layers only)
docker build --platform linux/amd64 -q -t "$IMAGE" "$CTX" > /dev/null
[ "$1" = "--image" ] && exit 0
ENVS=""
for v in BT3_VITA_DEV BT3_INCREMENTAL BT3_VITA_TITLE_ID BT3_VITA_TITLE BT3_VITA_VPK; do
    eval "val=\${$v-}"
    [ -n "$val" ] && ENVS="$ENVS -e $v" # (by name: docker takes the value from here, spaces and all)
done
# shellcheck disable=SC2086
exec docker run --rm --platform linux/amd64 $ENVS -v "$ROOT":/src -w /src "$IMAGE" python3 port/vita/build.py "$@"
