#!/usr/bin/env python3
"""One test run of the development build on the console through vita-agent-use
(https://github.com/shoui520/vita-agent-use): the same job as vita_console_run.sh (vitacompanion), with what
vita-agent-use adds: real screenshots during the run, the console's CPU / FPS / memory samples, and crash dumps seen
as they are written. CLAUDE.md and docs/vita/testing.md say how to set it up; vita-agent-use's own AGENTS.md is the
law for using it (its `serve` must run for the whole session; a person approves pairing and any change of tai/).

    python3 port/vita/tools/vita_agent_run.py --settings F --seconds N [--eboot] [--shots K] [--out DIR]

    --settings  a bt3.txt for this run, put in ux0:data/bt3/bt3.txt (the console's is downloaded first and put back
                after; if there was none, the run's is moved to the plugin's trash). exitafter=N is added to it, so
                the game ends itself and writes the line the run waits for.
    --seconds   the run's length (default 300)
    --eboot     first send port/build/vita_dev/eboot.bin over the installed one (the run's own "prepare" step). Each
                overwrite leaves a full hidden copy of the old file that no command can free (vita-agent-use's upload
                storage): send it only when it changed, and watch ux0's free space
    --shots     screenshots taken during the run, evenly spaced (default 6; 0 none)
    --title     default BTTD00008, the development build
    --out       default port/build/runs/agent-<date-time>
    env VITA_AGENT             the vita-agent launcher (default: vita-agent on the PATH)
        VITA_AGENT_CONFIG_DIR  the directory chosen in vita-agent-use's bootstrap

Leaves in --out: log.txt (this run's part of ux0:data/bt3/log-vgl.txt), shots/*.jpg, run.json and run_status.json,
events.jsonl (the run's events: performance samples, crash dumps, dialogs), hang.txt if any, perfsum.txt.
Runs over an hour (vita-agent-use's run timeout) are launched and watched without a run (no run-owned metrics).
"""
import argparse
import datetime
import json
import os
import pathlib
import shutil
import subprocess
import sys
import time

ROOT = pathlib.Path(__file__).resolve().parents[3]
LOG = "ux0:data/bt3/log-vgl.txt"
SETTINGS = "ux0:data/bt3/bt3.txt"
TERMINAL = {"completed", "cancelled", "failed", "connection_lost"}


def agent(*args, check=False):
    """Runs the vita-agent CLI; returns (exit code, the JSON objects it printed)."""
    exe = os.environ.get("VITA_AGENT") or shutil.which("vita-agent") or "vita-agent"
    p = subprocess.run([exe, *[str(a) for a in args]], capture_output=True, text=True)
    objs = []
    for line in p.stdout.splitlines():
        line = line.strip()
        if line.startswith("{"):
            try:
                objs.append(json.loads(line))
            except json.JSONDecodeError:
                pass
    if check and p.returncode != 0:
        raise RuntimeError("vita-agent %s failed (%d): %s %s" % (" ".join(map(str, args)), p.returncode, p.stdout[-600:], p.stderr[-300:]))
    return p.returncode, objs


def last(objs):
    finals = [o for o in objs if o.get("status") != "progress"]
    return finals[-1] if finals else (objs[-1] if objs else {})


def upload(src, dest, out, overwrite=False):
    """fs upload with a transfer state of its own: vita-agent-use keeps one per source and destination by default and
    takes a finished one with the same bytes as done again, without sending them (a file removed since stays gone)."""
    state = out / ("xfer-%d.json" % time.monotonic_ns())
    args = ["fs", "upload", src, dest, "--transfer-state", state] + (["--overwrite", "--yes"] if overwrite else [])
    rc, objs = agent(*args, check=True)
    return rc, objs


def exists(path):
    rc, objs = agent("fs", "stat", path)
    return rc == 0 and last(objs).get("status") == "ok"


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--settings", required=True, type=pathlib.Path)
    ap.add_argument("--seconds", type=int, default=300)
    ap.add_argument("--eboot", action="store_true")
    ap.add_argument("--shots", type=int, default=6)
    ap.add_argument("--title", default="BTTD00008")
    ap.add_argument("--out", type=pathlib.Path)
    o = ap.parse_args()
    out = o.out or ROOT / "port/build/runs" / ("agent-" + datetime.datetime.now().strftime("%Y%m%d-%H%M%S"))
    (out / "shots").mkdir(parents=True, exist_ok=True)
    note = lambda *a: print("vita_agent_run:", *a, flush=True)

    rc, objs = agent("server", "status")
    st = last(objs)
    if not (st.get("connected") or st.get("result", {}).get("connected")):
        note("vita-agent-use's serve is not connected (start `vita-agent serve` in the background first):", json.dumps(st)[:300])
        return 3
    agent("app", "close", o.title)

    # the settings: the console's kept, this run's sent (with exitafter, the end the run waits for)
    had = exists(SETTINGS)
    if had:
        agent("fs", "download", SETTINGS, out / "bt3.console.txt", check=True)
    run_settings = out / "bt3.run.txt"
    run_settings.write_text(o.settings.read_text() + "\nexitafter=%d\n" % o.seconds)
    upload(run_settings, SETTINGS, out, overwrite=had)
    if not exists(SETTINGS):
        note("the settings did not reach", SETTINGS)
        return 3

    eboot = ROOT / "port/build/vita_dev/eboot.bin"
    if o.eboot and not eboot.is_file():
        note(str(eboot), "not found (BT3_VITA_DEV=1 python3 port/vita/build.py)")
        return 2
    long_run = o.seconds + 180 > 3600
    t0 = time.time()
    if not long_run:
        spec = {"title_id": o.title, "timeout_s": o.seconds + 180, "performance_interval_ms": 2000, "close_on_completion": True,
                "completion": {"log": {"path": LOG, "literal": "vita: exitafter:"}}}
        if o.eboot:
            spec["prepare"] = {"upload": {"source": str(eboot), "destination": "ux0:app/%s/eboot.bin" % o.title, "overwrite": True}}
        (out / "run.json").write_text(json.dumps(spec, indent=2))
        rc, objs = agent("run", "start", out / "run.json", *(["--yes"] if o.eboot else []))
        run_id = last(objs).get("run_id") or last(objs).get("result", {}).get("run_id")
        if not run_id:
            note("the run did not start:", json.dumps(last(objs))[:400])
            return 3
        note("run", run_id, "for about", o.seconds, "s")
    else:
        if o.eboot:
            upload(eboot, "ux0:app/%s/eboot.bin" % o.title, out, overwrite=True)
        agent("server", "performance-start", "--seconds", 3600, "--interval-ms", 2000)
        agent("app", "launch", o.title, check=True)
        run_id = None
        note("launched for", o.seconds, "s (no run: longer than vita-agent-use's hour)")

    shots_at = [o.seconds * (k + 1) / (o.shots + 1) for k in range(o.shots)]
    status = {}
    while True:
        time.sleep(5)
        el = time.time() - t0
        while shots_at and el >= shots_at[0]:
            shots_at.pop(0)
            agent("screen", "capture", "--output", out / "shots" / ("%05d.jpg" % int(el)))
        if run_id:
            rc, objs = agent("run", "status")
            status = last(objs)
            phase = status.get("phase") or status.get("result", {}).get("phase")
            if phase in TERMINAL:
                break
        else:
            rc, objs = agent("app", "running")
            if el > 60 and o.title not in json.dumps(last(objs)):
                break
        if el > o.seconds + 600:
            note("no end after", int(el), "s: cancelling")
            agent("run", "cancel") if run_id else agent("app", "close", o.title)
            break
    (out / "run_status.json").write_text(json.dumps(status, indent=2))
    if not run_id:
        agent("server", "performance-stop")

    # the run's events (performance samples, crash dumps, dialogs)
    after, events = 0, []
    for _ in range(64):
        rc, objs = agent("server", "events", "--after", after, "--limit", 1024)
        r = last(objs)
        page = r.get("events") or r.get("result", {}).get("events") or []
        events += [e for e in page if run_id is None or e.get("run_id") in (run_id, None)]
        nxt = r.get("next") or r.get("result", {}).get("next")
        if not page or nxt is None or nxt == after:
            break
        after = nxt
    with open(out / "events.jsonl", "w") as f:
        for e in events:
            f.write(json.dumps(e) + "\n")

    # the log, the watchdog's file, the settings put back
    agent("fs", "download", LOG, out / "log-all.txt")
    if exists("ux0:data/bt3/hang-vgl.txt"):
        agent("fs", "download", "ux0:data/bt3/hang-vgl.txt", out / "hang.txt")
    if had:
        upload(out / "bt3.console.txt", SETTINGS, out, overwrite=True)
    else:
        agent("fs", "trash", SETTINGS, "--yes")
    if (out / "log-all.txt").is_file():
        lines = (out / "log-all.txt").read_text(encoding="utf-8", errors="replace").splitlines(True)
        start = max([i for i, l in enumerate(lines) if l.startswith("vita: Tenkaichi3Decomp")] or [0])
        (out / "log.txt").write_text("".join(lines[start:]))
        if any(l.startswith("soak: battle") for l in lines[start:]):
            for name, extra in (("perfsum.txt", []), ("perfsum_ab.txt", ["--ab"])):
                p = subprocess.run([sys.executable, str(ROOT / "port/vita/tools/perfsum.py"), *extra, str(out / "log.txt")], capture_output=True, text=True)
                (out / name).write_text(p.stdout + p.stderr)
    dumps = [e for e in events if str(e.get("type", "")).startswith("coredump")]
    note("phase:", status.get("phase") or status.get("result", {}).get("phase") or "(no run)",
         "| crash dumps:", len(dumps), "| shots:", len(list((out / "shots").glob("*.jpg"))), "| files in", out)
    return 0 if not dumps else 1


if __name__ == "__main__":
    sys.exit(main())
