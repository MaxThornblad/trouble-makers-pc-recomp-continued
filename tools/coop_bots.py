#!/usr/bin/env python3
"""Co-op bot tests: two scripted players in the real game, checked every frame.

Runs the headless build with scripted input for both players (replay
recording, so every run is deterministic) from save states, reads the
per-frame player trace (MM_P2_TRACE, src/game/player2.cpp) and checks:

  jumps      neither player's world position moves more than 24 px in a frame
             (except right after player 2 (re)joins)
  screen     both stay within the shared screen (|x| <= 181 px from the camera)
  holding    a player flagged as carrying always carries a live object
  rejoins    player 2 is not pulled back more often than the scenario allows
  present    player 2 is in the game for most of the run

Netplay scenarios run a host and a guest process over localhost and also
require identical memory fingerprints on both sides and no resync.

    tools/coop_bots.py --bin build_headless/src/game/troublemakers \
        --rom input/troublemakers.us1.z64 --home <HOME with savestates> [--only name]

Prints one line per scenario and ALL OK / FAILED at the end.
"""
import argparse
import os
import random
import re
import subprocess
import sys
import tempfile
import time
from pathlib import Path

TRACE = re.compile(
    r"\[p2trace\] (\d+) cam (-?\d+),(-?\d+) p1 (-?\d+),(-?\d+) st (\w+) fl (\w+) hold (\w+) \| "
    r"p2\((-?\d+)\) (-?\d+),(-?\d+) st (\w+) fl (\w+) hold (\w+)")
HELD = re.compile(r"\[p2trace\] held_ok (\d) (\d) hp (-?\d+) (-?\d+)")


def fuzz(seed, steps=300, grab_bias=False):
    rnd = random.Random(seed)
    keys = ["", "l", "r", "A", "B", "lA", "rA", "lB", "rB", "u", "d", "uB", "dB", "ll", "rr"]
    if grab_bias:
        keys += ["B", "B", "lB", "rB"]
    return ",".join("%d:%s" % (rnd.randint(3, 25), rnd.choice(keys)) for _ in range(steps))


# name, save slot, frames, player 1 script, player 2 script, allowed leash rejoins
SCENARIOS = [
    ("apart-1", 1, 700, "60:,600:r", "60:,600:l", 0),
    ("apart-4", 4, 700, "60:,600:l", "60:,600:r", 0),
    ("apart-6", 6, 700, "60:,600:r", "60:,600:l", 0),
    ("p1-carries-p2", 1, 500, "80:,6:l,10:B,20:,60:r,30:,60:l,20:,10:B,200:", "500:", 0),
    ("p2-carries-p1", 1, 500, "500:", "80:,6:r,10:B,20:,60:l,30:,60:r,20:,10:B,200:", 0),
    ("both-grab", 1, 400, "80:,6:l,10:B,100:,10:B,200:", "80:,6:r,10:B,100:,10:B,200:", 0),
    ("carry-jump-throw", 4, 500, "80:,6:r,10:B,20:,10:A,40:,30:r,10:B,300:", "500:", 0),
    ("p2-carry-jump", 4, 500, "500:", "80:,6:l,10:B,20:,10:A,40:,30:l,10:B,300:", 0),
]
for slot in (1, 2, 3, 4, 5, 6, 7, 8):
    for seed in (1, 2):
        SCENARIOS.append(("fuzz-%d-%d" % (slot, seed), slot, 2400,
                          fuzz(seed * 100 + slot, grab_bias=True), fuzz(seed * 100 + slot + 50, grab_bias=True), 3))

NETPLAY = [
    ("net-apart", 1, 1200, "60:,600:r,540:l", "60:,600:l,540:r"),
    ("net-carry", 1, 1200, "80:,6:l,10:B,20:,60:r,30:,60:l,20:,10:B,900:", "600:,6:r,10:B,20:,60:l,30:,60:r,20:,10:B,400:"),
    ("net-fuzz", 4, 1800, fuzz(901, grab_bias=True), fuzz(902, grab_bias=True)),
]

BASE_ENV = {"MM_HEADLESS_GFX": "1", "SDL_VIDEODRIVER": "dummy", "SDL_AUDIODRIVER": "dummy"}


def check_trace(text, allowed_rejoins):
    problems = []
    frames = present = 0
    prev = None
    joined_recently = 0
    held_bad = 0
    rejoins = text.count("rejoining")
    defeats = text.count("defeated, rejoining")
    for line in text.splitlines():
        if "joined in slot" in line or "rejoining" in line:
            joined_recently = 3
            continue
        m = HELD.match(line)
        if m:
            if m.group(1) == "0" or m.group(2) == "0":
                held_bad += 1
                if held_bad == 3:
                    problems.append("carrying flag without a live object (%s)" % line.strip())
            else:
                held_bad = 0
            continue
        m = TRACE.match(line)
        if not m:
            continue
        n = int(m.group(1))
        cx, cy = int(m.group(2)), int(m.group(3))
        p2slot = int(m.group(9))
        p1 = (cx + int(m.group(4)), cy + int(m.group(5)))
        p2 = (cx + int(m.group(10)), cy + int(m.group(11)))
        if p2slot < 0:
            prev = None
            continue
        frames += 1
        present += 1
        for who, rel in (("p1", int(m.group(4))), ("p2", int(m.group(10)))):
            if abs(rel) > 181 and joined_recently == 0:
                problems.append("%s off the shared screen at frame %d (x %d)" % (who, n, rel))
        if prev and joined_recently == 0:
            for who, a, b in (("p1", prev[0], p1), ("p2", prev[1], p2)):
                if abs(a[0] - b[0]) > 24 or abs(a[1] - b[1]) > 24:
                    problems.append("%s jumped at frame %d: %s -> %s" % (who, n, a, b))
        prev = (p1, p2)
        joined_recently = max(0, joined_recently - 1)
    if rejoins - defeats > allowed_rejoins:
        problems.append("%d leash rejoins (allowed %d)" % (rejoins - defeats, allowed_rejoins))
    if defeats > 4:
        problems.append("%d defeat rejoins" % defeats)
    if frames < 100:
        problems.append("player 2 present only %d traced frames" % frames)
    # de-duplicate noisy repeats
    seen, out = set(), []
    for p in problems:
        key = p.split(" at frame")[0]
        if key not in seen:
            seen.add(key)
            out.append(p)
    return out, frames


def run_local(args, name, slot, frames, pad1, pad2, allowed):
    with tempfile.TemporaryDirectory() as tmp:
        env = dict(os.environ, **BASE_ENV, HOME=args.home, MM_P2_TRACE="1",
                   MM_REPLAY_RECORD="%s/rec@200" % tmp, MM_REPLAY_LENGTH=str(frames),
                   MM_REPLAY_SPEED="4", MM_REPLAY_PAD=pad1, MM_REPLAY_PAD2=pad2,
                   MM_TEST_SAVESTATE_SLOT=str(slot), MM_TEST_SAVESTATE="load@150")
        timeout = 30 + frames // 40
        try:
            out = subprocess.run([args.bin, args.rom], env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                 timeout=timeout).stdout.decode(errors="replace")
        except subprocess.TimeoutExpired as e:
            out = (e.stdout or b"").decode(errors="replace")
    # Only the recorded part counts (from the recorder's load on).
    start = out.find("[replay] recording into")
    end = out.find("[replay] recording stopped")
    section = out[start:end if end > 0 else None] if start >= 0 else ""
    if start < 0:
        return ["did not start recording"], 0
    return check_trace(section, allowed)


def run_netplay(args, name, slot, frames, pad1, pad2, port):
    with tempfile.TemporaryDirectory() as tmp:
        guest_home = Path(tmp) / "guest"
        (guest_home / ".config" / "troublemakers-recomp").mkdir(parents=True)
        cfg = Path(args.home) / ".config" / "troublemakers-recomp"
        for f in ("troublemakers.n64.us.1.z64", "controls.json"):
            if (cfg / f).exists():
                (guest_home / ".config" / "troublemakers-recomp" / f).write_bytes((cfg / f).read_bytes())
        hlog, glog = Path(tmp) / "h.txt", Path(tmp) / "g.txt"
        common = dict(os.environ, **BASE_ENV, MM_NET_LENGTH=str(frames), MM_P2_TRACE="1")
        henv = dict(common, HOME=args.home, MM_TEST_SAVESTATE_SLOT=str(slot), MM_TEST_SAVESTATE="load@150",
                    MM_NET_HOST="%d@200" % port, MM_NET_PAD=pad1, MM_NET_LOG=str(hlog))
        genv = dict(common, HOME=str(guest_home), MM_NET_JOIN="127.0.0.1:%d@300" % port, MM_NET_PAD=pad2,
                    MM_NET_LOG=str(glog))
        timeout = 60 + frames // 30
        hfile = open(Path(tmp) / "host.log", "wb")   # a file, not a pipe: nobody reads it while the game runs
        host = subprocess.Popen([args.bin, args.rom], env=henv, stdout=hfile, stderr=subprocess.STDOUT)
        time.sleep(1)
        try:
            gout = subprocess.run([args.bin, args.rom], env=genv, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                  timeout=timeout).stdout.decode(errors="replace")
        except subprocess.TimeoutExpired as e:
            gout = (e.stdout or b"").decode(errors="replace")
        try:
            host.wait(timeout=20)
        except subprocess.TimeoutExpired:
            host.kill()
            host.wait()
        hfile.close()
        hout = (Path(tmp) / "host.log").read_bytes().decode(errors="replace")
        problems = []
        if "session started" not in hout or "joined the host's game" not in gout:
            return ["session did not start"], 0
        resyncs = hout.count("[netplay] resync:")
        if resyncs:
            problems.append("%d resyncs" % resyncs)
        h = [l.strip() for l in hlog.read_text().splitlines()] if hlog.exists() else []
        g = [l.strip() for l in glog.read_text().splitlines()] if glog.exists() else []
        common_n = min(len(h), len(g))
        if common_n < frames // 60 - 2:
            problems.append("only %d fingerprint checks" % common_n)
        if h[:common_n] != g[:common_n]:
            problems.append("fingerprints differ")
        start = hout.find("session started")
        more, traced = check_trace(hout[start:], 3)
        return problems + more, traced


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--bin", required=True)
    ap.add_argument("--rom", required=True)
    ap.add_argument("--home", required=True)
    ap.add_argument("--only", default="")
    ap.add_argument("--no-net", action="store_true")
    args = ap.parse_args()
    failed = 0
    port = 7900
    for name, slot, frames, p1, p2, allowed in SCENARIOS:
        if args.only and args.only not in name:
            continue
        problems, traced = run_local(args, name, slot, frames, p1, p2, allowed)
        print("%-18s %s (%d frames)%s" % (name, "ok  " if not problems else "FAIL", traced,
                                          "" if not problems else ": " + "; ".join(problems[:4])), flush=True)
        failed += bool(problems)
    if not args.no_net:
        for name, slot, frames, p1, p2 in NETPLAY:
            if args.only and args.only not in name:
                continue
            port += 1
            problems, traced = run_netplay(args, name, slot, frames, p1, p2, port)
            print("%-18s %s (%d frames)%s" % (name, "ok  " if not problems else "FAIL", traced,
                                              "" if not problems else ": " + "; ".join(problems[:4])), flush=True)
            failed += bool(problems)
    print("ALL OK" if failed == 0 else "FAILED: %d scenario(s)" % failed)
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
