# The development build

```sh
BT3_VITA_DEV=1 VITA_SAMPLES=~/vitasdk-samples python3 port/vita/build.py
```

builds `port/build/vita_dev/Tenkaichi3Decomp-Vita-dev.vpk`: the same program as the release, with its own title
(BTTD00008, "BT3 (development)") so that it can be installed next to it, and every setting of `bt3.txt` read. It
shares `ux0:data/bt3/` (the disc files, saves and settings) with the release and writes its log to
`ux0:data/bt3/log-vgl.txt`.

Started from its bubble without settings, it shows a start screen: the float self-test of the vector library's
arithmetic (the exact model against the sums computed on the PC, and the hardware model against the exact one),
then CROSS for the game, CIRCLE for the demo fight (computer against computer).

## `ux0:data/bt3/bt3.txt`

One `name=value` per line. The release reads only `disc`, `wide`, `heap` and `report`; the development build reads
all of them. The full list, each with a comment, is `read_settings` in `port/vita/vita_main.c`; the ones most used:

|---|---|

Most of the remaining settings switch one optimisation of the renderer off (`fastlane=0`, `stx=0`, `wtc=0`, ...)
so that its effect can be measured or a picture compared with and without it.

## The log

Each run appends to `log-vgl.txt` (the release: `log.txt`). With `report=1` it has, every two seconds: the frame
rate, where the time of each thread went (the game, the list reader, the drawer, the render thread), the uploads,
the texture and state caches, and the processor counters per thread when libperf is installed. When no picture has
been made for three seconds, a watchdog writes where every thread is to `hang-vgl.txt`: each thread's state, the
drawing and render threads' place in their work, the lists' two queues, the pictures shown, vitaGL's free memory and
the texture count.

## Where things are

|---|---|
