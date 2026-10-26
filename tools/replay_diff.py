#!/usr/bin/env python3
"""Compare two replay fingerprint files (hashes.bin) frame by frame.

usage: replay_diff.py <recorded hashes.bin> <playback hashes.bin>

Prints the first frame where game memory differed, which 64 KiB blocks
differed there, and how often each block differed over the whole run.
"""
import struct
import sys

BASE = 0x80000000
BLOCK = 0x10000
BLOCKS = 0x840000 // BLOCK
RECORD = 4 + 8 * BLOCKS


def load(path):
    data = open(path, "rb").read()
    frames = {}
    for i in range(len(data) // RECORD):
        rec = data[i * RECORD:(i + 1) * RECORD]
        frame = struct.unpack_from("<I", rec)[0]
        frames[frame] = struct.unpack_from("<%dQ" % BLOCKS, rec, 4)
    return frames


def main():
    a, b = load(sys.argv[1]), load(sys.argv[2])
    common = sorted(set(a) & set(b))
    print("frames: %d recorded, %d played, %d compared" % (len(a), len(b), len(common)))
    first = None
    counts = [0] * BLOCKS
    for f in common:
        diff = [i for i in range(BLOCKS) if a[f][i] != b[f][i]]
        for i in diff:
            counts[i] += 1
        if diff and first is None:
            first = (f, diff)
    if first is None:
        print("identical in every compared frame")
        return 0
    f, diff = first
    print("first difference at frame %d in %d block(s): %s" % (
        f, len(diff), " ".join("%08X" % (BASE + i * BLOCK) for i in diff)))
    print("blocks that ever differed (frames):")
    for i in sorted(range(BLOCKS), key=lambda i: -counts[i]):
        if counts[i]:
            print("  %08X  %d" % (BASE + i * BLOCK, counts[i]))
    return 1


if __name__ == "__main__":
    sys.exit(main())
