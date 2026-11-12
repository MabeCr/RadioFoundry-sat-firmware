#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
"""Satellite element sets by sound (docs/spec/sound-transfer.md): the reference encoder, in Python.

    tools/sat_sound.py amateur.txt -o sets.wav       a TLE file (3-line sets, or 2-line) as audio to play at the microphone
    tools/sat_sound.py --vectors                     write tests/data/sat_sound_vectors.json
    tools/sat_sound.py --check                       fail if that file differs from what this encoder gives now

The vectors are shared with the host test (tests/host/satSoundTest.c, the C frame code and HDLC encoder) and with the
satellite element set page's encoder (https://github.com/MabeCr/sat-tle-tools), so the three can't drift. After a change,
copy the file to the page's repository as test/sound_vectors.json."""
import argparse
import json
import math
import os
import random
import struct
import sys
import wave

HERE = os.path.dirname(os.path.abspath(__file__))
VECTORS = os.path.join(HERE, "..", "tests", "data", "sat_sound_vectors.json")

MAGIC = 0x54
VERSION = 1
NAME_MAX = 24
SETS_MAX = 32
TLE_LEN = 69
BAUD = 1200
MARK_HZ = 1200
SPACE_HZ = 2200
PREAMBLE_FLAGS = 30     # about 0.2 s of flags before each frame
TAIL_FLAGS = 3
GAP_MS = 100            # silence between frames
LEAD_MS = 500           # silence before the first frame
PASS_GAP_MS = 1000      # silence between the passes of a repeated set


def tle_checksum(line):
    return sum(int(c) if c.isdigit() else (1 if c == "-" else 0) for c in line[:68]) % 10


def tle_ok(l1, l2):
    """The same rules as core/sat/sat_tle.c's tle_parse, as far as a sender needs them"""
    if len(l1) != TLE_LEN or len(l2) != TLE_LEN or l1[0] != "1" or l2[0] != "2":
        return False
    if not (l1[68].isdigit() and l2[68].isdigit()):
        return False
    if tle_checksum(l1) != int(l1[68]) or tle_checksum(l2) != int(l2[68]):
        return False
    return l1[2:7] == l2[2:7]


def build_frame(sid, index, count, name, l1, l2):
    """One satellite's frame; None where the C build (sat_sound_build) returns 0"""
    nb = name.encode("ascii", "strict")
    if not (0 < count <= SETS_MAX and 0 <= index < count and len(nb) <= NAME_MAX):
        return None
    if any(c < 0x20 or c > 0x7E for c in nb) or not tle_ok(l1, l2):
        return None
    return bytes([MAGIC, VERSION, sid & 255, index, count, len(nb)]) + nb + l1.encode() + l2.encode()


def fcs(data):
    crc = 0xFFFF
    for b in data:
        crc ^= b
        for _ in range(8):
            crc = (crc >> 1) ^ 0x8408 if crc & 1 else crc >> 1
    return ~crc & 0xFFFF


class Hdlc:
    """core/aprs/hdlc.c's encoder: NRZI (a 0 changes the tone), bit stuffing, LSB first; tone 1 = mark"""
    def __init__(self):
        self.tones = []
        self.level = 1
        self.ones = 0

    def _put(self, bit):
        if not bit:
            self.level ^= 1
        self.tones.append(self.level)

    def flags(self, n):
        for _ in range(n):
            for i in range(8):
                self._put((0x7E >> i) & 1)
        self.ones = 0

    def _byte(self, b):
        for i in range(8):
            bit = (b >> i) & 1
            self._put(bit)
            if bit:
                self.ones += 1
                if self.ones == 5:
                    self._put(0)
                    self.ones = 0
            else:
                self.ones = 0

    def frame(self, data):
        c = fcs(data)
        self.ones = 0
        for b in data:
            self._byte(b)
        self._byte(c & 255)
        self._byte(c >> 8)


def tones_packed(tones):
    """The tone bits as hdlc.h keeps them: bit i of the buffer is byte i // 8, bit i % 8"""
    out = bytearray((len(tones) + 7) // 8)
    for i, t in enumerate(tones):
        if t:
            out[i >> 3] |= 1 << (i & 7)
    return bytes(out)


def frame_tones(frame, preamble=PREAMBLE_FLAGS, tail=TAIL_FLAGS):
    h = Hdlc()
    h.flags(preamble)
    h.frame(frame)
    h.flags(tail)
    return h.tones


def parse_tle_file(text):
    """(name, line1, line2) for each set in a 3-line or a 2-line file"""
    lines = [ln.rstrip() for ln in text.splitlines() if ln.strip()]
    sets, i = [], 0
    while i < len(lines):
        if lines[i].startswith("1 ") and i + 1 < len(lines) and lines[i + 1].startswith("2 "):
            sets.append(("", lines[i], lines[i + 1]))
            i += 2
        elif i + 2 < len(lines) and lines[i + 1].startswith("1 ") and lines[i + 2].startswith("2 "):
            sets.append((lines[i].strip()[:NAME_MAX], lines[i + 1], lines[i + 2]))
            i += 3
        else:
            i += 1
    return sets


def clean_name(name):
    return "".join(c if 0x20 <= ord(c) <= 0x7E else "?" for c in name)[:NAME_MAX]


def audio(sets, sid, passes=2, rate=48000, amp=0.6):
    """The samples (floats, -1..1) of a whole transfer: every set's frame, the whole list sent `passes` times"""
    frames = []
    for i, (name, l1, l2) in enumerate(sets):
        f = build_frame(sid, i, len(sets), clean_name(name), l1, l2)
        if f is None:
            raise ValueError("set %d (%s) is not a valid TLE" % (i, name))
        frames.append(f)
    out = [0.0] * int(rate * LEAD_MS / 1000)
    phase = 0.0
    for p in range(passes):
        if p:
            out += [0.0] * int(rate * PASS_GAP_MS / 1000)
        for f in frames:
            for t in frame_tones(f):
                hz = MARK_HZ if t else SPACE_HZ
                step = 2.0 * math.pi * hz / rate
                for _ in range(int(round(rate / BAUD))):
                    out.append(amp * math.sin(phase))
                    phase += step
                phase %= 2.0 * math.pi
            out += [0.0] * int(rate * GAP_MS / 1000)
    return out


def write_wav(path, samples, rate=48000):
    with wave.open(path, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(rate)
        w.writeframes(b"".join(struct.pack("<h", max(-32768, min(32767, int(round(s * 32767))))) for s in samples))


# --- the shared vectors ---

def fix(line):
    return line[:68] + str(tle_checksum(line))


ISS1 = fix("1 25544U 98067A   26300.51782528  .00016717  00000-0  30144-3 0  999")
ISS2 = fix("2 25544  51.6416 247.4627 0006703 130.5360 325.0288 15.4981536147455")
SO1 = fix("1 27607U 02058C   26301.20512011  .00000825  00000-0  11606-3 0  999")
SO2 = fix("2 27607  64.5560 140.2232 0048032 102.6391 257.9944 14.7939185426102")


def vectors():
    cases = [("ISS (ZARYA)", ISS1, ISS2, 7, 0, 2), ("SO-50", SO1, SO2, 7, 1, 2), ("", ISS1, ISS2, 255, 31, 32)]
    good = []
    for name, l1, l2, sid, index, count in cases:
        f = build_frame(sid, index, count, name, l1, l2)
        h = Hdlc()
        h.flags(4)
        h.frame(f)
        h.flags(2)
        good.append({"name": name, "line1": l1, "line2": l2, "id": sid, "index": index, "count": count,
                     "frame_hex": f.hex(), "tones_hex": tones_packed(h.tones).hex(), "tone_bits": len(h.tones)})
    bad_l1 = ISS1[:68] + str((int(ISS1[68]) + 1) % 10)
    refused = [
        {"why": "check digit", "name": "X", "line1": bad_l1, "line2": ISS2, "id": 1, "index": 0, "count": 1},
        {"why": "different satellites", "name": "X", "line1": ISS1, "line2": SO2, "id": 1, "index": 0, "count": 1},
        {"why": "index at the count", "name": "X", "line1": ISS1, "line2": ISS2, "id": 1, "index": 2, "count": 2},
        {"why": "count over 32", "name": "X", "line1": ISS1, "line2": ISS2, "id": 1, "index": 0, "count": 33},
        {"why": "name too long", "name": "N" * 25, "line1": ISS1, "line2": ISS2, "id": 1, "index": 0, "count": 1},
        {"why": "name not ASCII", "name": "café", "line1": ISS1, "line2": ISS2, "id": 1, "index": 0, "count": 1},
    ]
    for r in refused:
        try:
            assert build_frame(r["id"], r["index"], r["count"], r["name"], r["line1"], r["line2"]) is None
        except UnicodeEncodeError:
            pass
    refused = [r for r in refused if r["why"] != "name not ASCII"]
    return {
        "about": "tools/sat_sound.py --vectors: frames (frame_hex) and their HDLC tone bits (4 flags, the frame, 2 flags; "
                 "tones_hex is hdlc.h's packing, bit i = byte i // 8, bit i % 8) that the C code and the page must reproduce; "
                 "'refused' are arguments no encoder may build a frame for",
        "good": good,
        "refused": refused,
    }


def dump():
    return json.dumps(vectors(), indent=1) + "\n"


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("tle", nargs="?", help="a TLE file (CelesTrak or AMSAT, 3-line or 2-line sets)")
    ap.add_argument("-o", "--output", help="the WAV file to write")
    ap.add_argument("--id", type=int, default=None, help="the transfer id 0-255 (random)")
    ap.add_argument("--passes", type=int, default=2, help="times the whole set is sent (2)")
    ap.add_argument("--rate", type=int, default=48000, help="sample rate (48000)")
    ap.add_argument("--vectors", action="store_true", help="write the shared test vectors")
    ap.add_argument("--check", action="store_true", help="fail if the vectors file is out of date")
    a = ap.parse_args()
    if a.vectors or a.check:
        text = dump()
        if a.vectors:
            with open(VECTORS, "w") as f:
                f.write(text)
            print("sat_sound_vectors.json: written")
            return 0
        try:
            with open(VECTORS) as f:
                cur = f.read()
        except OSError:
            cur = ""
        if cur != text:
            print("sat_sound_vectors.json: out of date: run tools/sat_sound.py --vectors")
            return 1
        print("sat_sound_vectors.json: current")
        return 0
    if not a.tle or not a.output:
        ap.error("give a TLE file and -o OUTPUT.wav")
    with open(a.tle) as f:
        sets = parse_tle_file(f.read())
    sets = [s for s in sets if tle_ok(s[1], s[2])]
    if not sets:
        print("no valid TLE sets in", a.tle)
        return 1
    if len(sets) > SETS_MAX:
        print("%d sets: only the first %d are sent (the radio stores %d)" % (len(sets), SETS_MAX, SETS_MAX))
        sets = sets[:SETS_MAX]
    sid = a.id if a.id is not None else random.randrange(256)
    samples = audio(sets, sid, a.passes, a.rate)
    write_wav(a.output, samples, a.rate)
    print("%d satellites, transfer %d, %d pass(es), %.0f seconds: %s" % (len(sets), sid, a.passes, len(samples) / a.rate,
                                                                    a.output))
    return 0


if __name__ == "__main__":
    sys.exit(main())
