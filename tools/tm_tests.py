#!/usr/bin/env python3
"""Trouble Makers tests, second kit: named save states, one check per test.

The kit is a folder (never committed: it holds the owner's save states):
    <kit>/home/      a test home: .config/troublemakers-recomp/ with cheats.cfg,
                     display.cfg, controls.json, saves/ (the ROM is passed in)
    <kit>/states/    <name>.tmstate, the owner's states renamed
    <kit>/STATES.md  what each state is and where it came from

Each test loads its state into slot 0 of a fresh copy of the home, plays two
pad scripts (frames:buttons runs, as coop_bots.py) and checks what it is
about. Tests carry tags; run a tag with --only (e.g. --only cat,split).

    python3 tools/tm_tests.py --bin build_headless/src/game/troublemakers \\
        --rom input/troublemakers.us1.z64 --kit /path/to/kit [--only tag,name]
"""
import argparse
import os
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

TRACE = re.compile(
    r"\[p2trace\] (\d+) cam (-?\d+),(-?\d+) p1 (-?\d+),(-?\d+) st (\w+) fl (\w+) hold (\w+) \| "
    r"p2\((-?\d+)\) (-?\d+),(-?\d+) st (\w+) fl (\w+) hold (\w+)")
FAR = re.compile(r"\[far\] camera (-?\d+),(-?\d+) second (-?\d+),(-?\d+) \| p1 (-?\d+),(-?\d+) p2 (-?\d+),(-?\d+)")
DUMP = re.compile(r"\[dump\] t(\d+) slot +(\d+) type (\w+) st (\w+) fl (\w+) pos (-?\d+),(-?\d+) hp (-?\d+) img (\w+)")
SEATED = 0x003


class Run:
    """One game run's output, read for the checks."""

    def __init__(self, out):
        self.out = out
        start = out.find("[replay] recording into")
        end = out.find("[replay] recording stopped")
        self.section = out[start:end if end > 0 else None] if start >= 0 else ""
        self.frames = []   # (n, p1 state, p2 state, p1 x, p2 x) in the game camera's terms
        for m in TRACE.finditer(self.section):
            self.frames.append((int(m.group(1)), int(m.group(6), 16) & 0xFFF, int(m.group(12), 16) & 0xFFF,
                                int(m.group(4)), int(m.group(10))))
        self.far = [tuple(int(v) for v in m.groups()) for m in FAR.finditer(self.section)]
        self.dumps = [m.groups() for m in DUMP.finditer(out)]

    def lines(self, text):
        return self.section.count(text)


def seated_throughout(who, after=10):
    def check(r):
        if not r.frames:
            return ["no traced frames"]
        k = 1 if who == 1 else 2
        bad = [f[0] for f in r.frames if f[0] >= after and f[k] != SEATED]
        return ["player %d not seated on %d frames (first %d)" % (who, len(bad), bad[0])] if bad else []
    return check


def seated_at_end(who):
    def check(r):
        if not r.frames:
            return ["no traced frames"]
        k = 1 if who == 1 else 2
        st = r.frames[-1][k]
        return [] if st == SEATED else ["player %d ends in state %03X, not seated" % (who, st)]
    return check


def apart_at_end(min_px):
    def check(r):
        if not r.far:
            return ["no far-apart lines (split screen off?)"]
        p1x, p2x = r.far[-1][4], r.far[-1][6]
        return [] if abs(p1x - p2x) >= min_px else ["only %d px apart at the end (want %d)" % (abs(p1x - p2x), min_px)]
    return check


def p2_stays_near(start_px, max_drift):
    """Player 2 idle: her world x stays within max_drift of where she was."""
    def check(r):
        if not r.far:
            return ["no far-apart lines (split screen off?)"]
        drift = abs(r.far[-1][6] - start_px)
        return [] if drift <= max_drift else ["player 2 drifted %d px standing still" % drift]
    return check


def near_at_end(max_px):
    def check(r):
        if not r.frames:
            return ["no traced frames"]
        d = abs(r.frames[-1][3] - r.frames[-1][4])
        return [] if d <= max_px else ["%d px apart at the end (want <= %d)" % (d, max_px)]
    return check


def logged(*texts):
    def check(r):
        return ["no '%s'" % t for t in texts if t not in r.section]
    return check


def no_dust(after):
    """No cat dust clouds (type 0x0009, picture 0x1D8) in the dumps after a frame."""
    def check(r):
        n = sum(1 for d in r.dumps if int(d[0]) >= after and d[2] == "0009" and d[8] == "01D8")
        return ["%d dust clouds" % n] if n else []
    return check


#: (name, tags, state, frames, pad 1, pad 2, extra env, checks)
TESTS = [
    # The cat's dust clouds drifted ahead of the ride outside 3-12 ("flames").
    ("cat-no-flames", "cat", "cat-ride-flames", 400, "150:,250:r", "400:", {"MM_TEST_DUMP": "250,300,350"},
     [no_dust(250)]),
    # Split screen on cats: the riders stayed seated only one frame in three
    # (blinking, standing on the cat).
    ("split-seated-idle", "cat,split", "split-blink", 400, "400:", "400:", {},
     [seated_throughout(1), seated_throughout(2), p2_stays_near(488, 8)]),
    ("split-seated-saved-ride", "cat,split", "split-cat-both", 400, "400:", "400:", {},
     [seated_throughout(1), seated_throughout(2)]),
    # ... and player 2 riding away from player 1 (she was towed after him).
    ("split-ride-apart", "cat,split", "split-blink", 400, "60:,340:r", "60:,340:l", {},
     [seated_throughout(1), seated_throughout(2), apart_at_end(900)]),
    ("split-ride-apart-other-way", "cat,split", "split-blink", 400, "60:,340:l", "60:,340:r", {},
     [seated_throughout(1), seated_throughout(2), apart_at_end(900)]),
    # Grabbing a rider: she gets off her cat, is carried, and her cat comes
    # back when she stands again (put down or thrown).
    ("grab-rider-put-down", "cat,grab,split", "split-blink", 400, "60:,4:l,8:B,60:,8:B,260:", "400:", {},
     [logged("[mount] player 2 gets off the cat: carried", "[mount] player 2's own cat"),
      seated_at_end(1), seated_at_end(2), near_at_end(40)]),
    ("grab-rider-throw", "cat,grab,split", "split-blink", 400, "60:,4:l,8:B,40:,8:lB,280:", "400:", {},
     [logged("[mount] player 2 gets off the cat: carried", "[mount] player 2's own cat"),
      seated_at_end(1), seated_at_end(2)]),
    ("grab-rider-p2-throws-p1", "cat,grab,split", "split-blink", 400, "400:", "60:,4:r,8:B,40:,8:rB,280:", {},
     [logged("[mount] player 1 gets off the cat: carried", "[mount] player 1's own cat"),
      seated_at_end(1), seated_at_end(2)]),
    ("grab-rider-one-screen", "cat,grab", "split-blink", 400, "60:,4:l,8:B,60:,8:B,260:", "400:",
     {"MM_TEST_CHEATS": "split_screen=0"},
     [logged("[mount] player 2 gets off the cat: carried", "[mount] player 2's own cat"),
      seated_at_end(1), seated_at_end(2), near_at_end(40)]),
    # The owner's state: player 2 thrown while seated hung in the throw's
    # flight state on her cat for good.
    ("grab-stuck-throw-frees", "cat,grab,split", "split-cat-grab", 400, "400:", "400:", {},
     [seated_at_end(2)]),
]


def run(args, test):
    name, tags, state, frames, pad1, pad2, env_extra, checks = test
    kit = Path(args.kit)
    src_state = kit / "states" / (state + ".tmstate")
    if not src_state.exists():
        return None, "no state '%s' in the kit" % state
    with tempfile.TemporaryDirectory() as tmp:
        home = Path(tmp) / "home"
        shutil.copytree(kit / "home", home)
        slot0 = home / ".config" / "troublemakers-recomp" / "savestates" / "slot0.tmstate"
        slot0.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(src_state, slot0)
        env = dict(os.environ, MM_HEADLESS_GFX="1", SDL_VIDEODRIVER="dummy", SDL_AUDIODRIVER="dummy",
                   HOME=str(home), MM_P2_TRACE="1", MM_FAR_DEBUG="1",
                   MM_REPLAY_RECORD="%s/rec@200" % tmp, MM_REPLAY_LENGTH=str(frames), MM_REPLAY_SPEED="4",
                   MM_REPLAY_PAD=pad1, MM_REPLAY_PAD2=pad2,
                   MM_TEST_SAVESTATE_SLOT="0", MM_TEST_SAVESTATE="load@150")
        env.update(env_extra)
        try:
            out = subprocess.run([args.bin, args.rom], env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                 timeout=60 + frames // 20).stdout.decode(errors="replace")
        except subprocess.TimeoutExpired as e:
            out = (e.stdout or b"").decode(errors="replace")
    if args.keep:
        d = Path(args.keep) / name
        d.mkdir(parents=True, exist_ok=True)
        (d / "game.log").write_text(out)
    r = Run(out)
    if not r.section:
        return ["did not start recording"], None
    problems = []
    for c in checks:
        problems += c(r)
    return problems, None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--bin", required=True)
    ap.add_argument("--rom", required=True)
    ap.add_argument("--kit", required=True)
    ap.add_argument("--only", default="", help="tags or test names, comma-separated")
    ap.add_argument("--keep", default="", help="keep each test's log under DIR/<name>/")
    args = ap.parse_args()
    want = [w for w in args.only.split(",") if w]
    failed = 0
    for test in TESTS:
        name, tags = test[0], test[1].split(",")
        if want and not any(w == name or w in tags for w in want):
            continue
        problems, skip = run(args, test)
        if skip:
            print("%-28s skip (%s)" % (name, skip), flush=True)
            continue
        print("%-28s %s%s" % (name, "ok" if not problems else "FAIL", "" if not problems else ": " + "; ".join(problems[:4])),
              flush=True)
        failed += bool(problems)
    print("ALL OK" if failed == 0 else "FAILED: %d test(s)" % failed)
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
