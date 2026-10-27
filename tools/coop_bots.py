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

# Far apart (far_coop.cpp), run locally with MM_FAR=1: once as player 1's
# machine sees it and once building player 2's picture each frame
# (MM_FAR_SHADOW=1), which must not change the game at all (same memory
# fingerprints every frame). Name, slot, frames, scripts, px apart wanted.
FAR = [
    ("far-apart-1", 1, 900, "60:,840:r", "60:,840:l", 600),
    ("far-apart-4", 4, 900, "60:,840:l", "60:,840:r", 0),
    ("far-apart-6", 6, 900, "60:,840:r", "60:,840:l", 0),
    ("far-there-and-back", 1, 1200, "60:,500:r,600:", "60:,300:l,300:r,500:", 400),
    ("far-fuzz-4", 4, 1800, fuzz(404, grab_bias=True), "60:," + fuzz(405), 0),
    ("far-fuzz-2", 2, 1800, fuzz(202), "60:,300:l," + fuzz(203), 0),
]

NETPLAY = [
    ("net-apart", 1, 1200, "60:,600:r,540:l", "60:,600:l,540:r"),
    ("net-far", 1, 1500, "60:,1440:r", "60:,1440:l"),
    ("net-steam", 1, 1200, "60:,600:r,540:l", "60:,600:l,540:r"),   # over the Steam stand-in (--steam-mock)
    ("net-carry", 1, 1200, "80:,6:l,10:B,20:,60:r,30:,60:l,20:,10:B,900:", "600:,6:r,10:B,20:,60:l,30:,60:r,20:,10:B,400:"),
    ("net-fuzz", 4, 1800, fuzz(901, grab_bias=True), fuzz(902, grab_bias=True)),
]

BASE_ENV = {"MM_HEADLESS_GFX": "1", "SDL_VIDEODRIVER": "dummy", "SDL_AUDIODRIVER": "dummy"}


# States in which a player's position is the carry's to set: being carried
# (0x16 grabbed, 0x2F; she snaps to the carrier's hands, and to the other side when the
# carrier turns), reaching (0x11F), carrying / throwing (0x122, 0x126).
CARRY_STATES = {0x16, 0x2F, 0x11F, 0x122, 0x126}


def check_trace(text, allowed_rejoins, far=False):
    problems = []
    frames = present = apart = 0
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
        states = (int(m.group(6), 16) & 0xFFF, int(m.group(12), 16) & 0xFFF)
        p2slot = int(m.group(9))
        p1 = (cx + int(m.group(4)), cy + int(m.group(5)))
        p2 = (cx + int(m.group(10)), cy + int(m.group(11)))
        if p2slot < 0:
            prev = None
            continue
        frames += 1
        present += 1
        apart = max(apart, abs(p1[0] - p2[0]))
        if not far:
            for who, rel in (("p1", int(m.group(4))), ("p2", int(m.group(10)))):
                if abs(rel) > 181 and joined_recently == 0:
                    problems.append("%s off the shared screen at frame %d (x %d)" % (who, n, rel))
        if prev and joined_recently == 0:
            for k, who, a, b in ((0, "p1", prev[0], p1), (1, "p2", prev[1], p2)):
                if states[k] in CARRY_STATES or prev[2][k] in CARRY_STATES:
                    continue
                # Landing (a new state) may snap her up onto what she lands on.
                landing = states[k] != prev[2][k] and abs(a[0] - b[0]) <= 8 and 0 < b[1] - a[1] <= 32
                if not landing and (abs(a[0] - b[0]) > 24 or abs(a[1] - b[1]) > 24):
                    problems.append("%s jumped at frame %d: %s -> %s" % (who, n, a, b))
        prev = (p1, p2, states)
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
    return out, frames, apart


def run_local(args, name, slot, frames, pad1, pad2, allowed, extra_env=None, far=False, want_apart=0):
    with tempfile.TemporaryDirectory() as tmp:
        env = dict(os.environ, **BASE_ENV, HOME=args.home, MM_P2_TRACE="1",
                   MM_REPLAY_RECORD="%s/rec@200" % tmp, MM_REPLAY_LENGTH=str(frames),
                   MM_REPLAY_SPEED="4", MM_REPLAY_PAD=pad1, MM_REPLAY_PAD2=pad2,
                   MM_TEST_SAVESTATE_SLOT=str(slot), MM_TEST_SAVESTATE="load@150", **(extra_env or {}))
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
    problems, traced, apart = check_trace(section, allowed, far)
    if want_apart and apart < want_apart:
        problems.append("only %d px apart at most (want %d)" % (apart, want_apart))
    return problems, traced


def run_far(args, name, slot, frames, pad1, pad2, want_apart):
    hashes = []
    problems = []
    traced = 0
    for shadow in (False, True):
        with tempfile.TemporaryDirectory() as tmp:
            env = dict(os.environ, **BASE_ENV, HOME=args.home, MM_P2_TRACE="1", MM_FAR="1",
                       MM_REPLAY_RECORD="%s/rec@200" % tmp, MM_REPLAY_LENGTH=str(frames),
                       MM_REPLAY_SPEED="4", MM_REPLAY_PAD=pad1, MM_REPLAY_PAD2=pad2,
                       MM_TEST_SAVESTATE_SLOT=str(slot), MM_TEST_SAVESTATE="load@150")
            if shadow:
                env["MM_FAR_SHADOW"] = "1"
            try:
                out = subprocess.run([args.bin, args.rom], env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                     timeout=40 + frames // 20).stdout.decode(errors="replace")
            except subprocess.TimeoutExpired as e:
                out = (e.stdout or b"").decode(errors="replace")
            h = Path(tmp) / "rec" / "hashes.bin"
            hashes.append(h.read_bytes() if h.exists() else b"")
        start = out.find("[replay] recording into")
        end = out.find("[replay] recording stopped")
        if start < 0:
            return ["did not start recording"], 0
        if shadow:
            problems, traced, apart = check_trace(out[start:end if end > 0 else None], 3, far=True)
            if want_apart and apart < want_apart:
                problems.append("only %d px apart at most (want %d)" % (apart, want_apart))
            if "shadow frame too big" in out:
                problems.append("shadow frame overflowed its mirror")
    n = min(len(hashes[0]), len(hashes[1]))
    if n == 0 or abs(len(hashes[0]) - len(hashes[1])) > 0:
        problems.append("recordings differ in length (%d / %d bytes)" % (len(hashes[0]), len(hashes[1])))
    elif hashes[0][:n] != hashes[1][:n]:
        first = next(i for i in range(n) if hashes[0][i] != hashes[1][i])
        problems.append("player 2's picture changed the game (fingerprints differ from byte %d)" % first)
    return problems, traced


def run_netplay(args, name, slot, frames, pad1, pad2, port):
    steam = name.startswith("net-steam")
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
        if steam:
            # Two Steam users who are friends; the guest finds the host's lobby
            # through the friends list (tools/steam_mock/mock_steam.c).
            host_id, guest_id = "76561190000000%03d" % (port % 1000), "76561190000001%03d" % (port % 1000)
            mock = dict(MM_STEAM_LIB=args.steam_mock, MOCK_STEAM_DIR=str(Path(tmp) / "steam"))
            henv.update(mock, MM_NET_HOST="steam@200", MOCK_STEAM_ID=host_id, MOCK_STEAM_FRIENDS=guest_id)
            genv.update(mock, MM_NET_JOIN="steam@300", MOCK_STEAM_ID=guest_id, MOCK_STEAM_FRIENDS=host_id)
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
        more, traced, _ = check_trace(hout[start:], 3, far=True)
        return problems + more, traced


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--bin", required=True)
    ap.add_argument("--rom", required=True)
    ap.add_argument("--home", required=True)
    ap.add_argument("--only", default="")
    ap.add_argument("--no-net", action="store_true")
    ap.add_argument("--steam-mock", default="", help="the Steam stand-in library (cc -shared -fPIC "
                    "-o libsteam_api.so tools/steam_mock/mock_steam.c); without it net-steam is skipped")
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
    for name, slot, frames, p1, p2, want in FAR:
        if args.only and args.only not in name:
            continue
        problems, traced = run_far(args, name, slot, frames, p1, p2, want)
        print("%-18s %s (%d frames)%s" % (name, "ok  " if not problems else "FAIL", traced,
                                          "" if not problems else ": " + "; ".join(problems[:4])), flush=True)
        failed += bool(problems)
    if not args.no_net:
        for name, slot, frames, p1, p2 in NETPLAY:
            if args.only and args.only not in name:
                continue
            if name.startswith("net-steam") and not args.steam_mock:
                print("%-18s skip (no --steam-mock)" % name, flush=True)
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
