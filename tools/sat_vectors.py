#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
"""The test vectors the host tool (tools/rfoundry_debug.py) shares with the satellite element set page
(https://github.com/MabeCr/sat-tle-tools, docs/dev/sat-tle-web-tool.md): the Python code works the outcomes out, and the
page's Node tests must agree with them, so the two implementations of the update rules can't drift.
    tools/sat_vectors.py          write tests/data/sat_update_vectors.json
    tools/sat_vectors.py --check  fail if that file differs from what the Python tool gives now (the tool's rules changed)
After a change, copy the file to the page's repository as test/vectors.json."""
import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import rfoundry_debug as d  # noqa: E402
VECTORS = os.path.join(HERE, "..", "tests", "data", "sat_update_vectors.json")


def fix(line):
    return line[:68] + str(d.tle_checksum(line))


ISS1 = fix("1 25544U 98067A   26300.51782528  .00016717  00000-0  30144-3 0  999")
ISS2 = fix("2 25544  51.6416 247.4627 0006703 130.5360 325.0288 15.4981536147455")
NEWER1 = fix("1 25544U 98067A   26307.51782528  .00016717  00000-0  30144-3 0  999")
SO1 = fix("1 27607U 02058C   26301.20512011  .00000825  00000-0  11606-3 0  999")
SO2 = fix("2 27607  64.5560 140.2232 0048032 102.6391 257.9944 14.7939185426102")
RS1 = fix("1 44909U 19096E   26306.40000000  .00000100  00000-0  10000-4 0  999")
RS2 = fix("2 44909  82.5200  10.0000 0001000   0.0000  90.0000 12.7000000000010")


class FakeStore:
    """tools/rfoundry_debug.py's own self-test store, with the rows kept: the radio's side of sat_update"""
    def __init__(self, rows):
        self.rows = [dict(r) for r in rows]
        self.loaded = None

    def sat_list(self):
        return list(enumerate(self.rows))

    def sat_load(self, l1, l2):
        self.loaded = (d.tle_catalog(l1), int(d.tle_epoch(l1)))
        return 0, 0, self.loaded[0]

    def sat_save(self, *args):
        norad, epoch = self.loaded
        keys = ("up_hz", "down_hz", "up_tone", "arm_tone", "arm_ms", "armed_s", "name")
        prof = dict(zip(keys, args)) if args else {k: 0 for k in keys}
        row = {"norad": norad, "elements": True, "epoch": epoch, **prof}
        for i, r in enumerate(self.rows):
            if r["norad"] == norad:
                self.rows[i] = row
                return
        self.rows.append(row)

    def view(self, sections=None):
        return {"sat.profile": "-"}


def outcome(line):
    for word, key in (("already current", "current"), ("not in the file", "missing"), ("would update", "would"),
                      ("not loaded", "failed"), ("updated", "updated"), ("stored", "stored")):
        if word in line:
            return key
    return "?"


def norad_of(line):
    """The catalog number in one of sat_update's log lines: a name (11 columns, may hold a space), then the number"""
    return int(line[12:18])


def build():
    text = f"ISS (ZARYA)\n{NEWER1}\n{ISS2}\nSO-50\n{SO1}\n{SO2}\nRS-44\n{RS1}\n{RS2}\n{ISS1}\nbad line\n"
    tl = d.parse_tles(text)
    rows = [{"norad": 25544, "elements": True, "epoch": int(d.tle_epoch(ISS1)), "up_hz": 145990000, "down_hz": 437800000,
             "up_tone": 670, "arm_tone": 0, "arm_ms": 0, "armed_s": 0, "name": "ISS"},
            {"norad": 27607, "elements": True, "epoch": int(d.tle_epoch(SO1)), "up_hz": 145850000, "down_hz": 436795000,
             "up_tone": 670, "arm_tone": 744, "arm_ms": 2000, "armed_s": 600, "name": "SO-50"},
            {"norad": 43017, "elements": False, "epoch": 0, "up_hz": 0, "down_hz": 0, "up_tone": 0, "arm_tone": 0,
             "arm_ms": 0, "armed_s": 0, "name": "AO-91"}]
    run = {}
    for key, add, dry in (("update", [44909, 99999], False), ("dry_run", [], True)):
        store = FakeStore(rows)
        lines = []
        failed = d.sat_update(store, tl, add=add, dry_run=dry, log=lines.append)
        run[key] = {"add": add, "dry_run": dry, "failed": failed,
                    "outcomes": [[norad_of(ln), outcome(ln)] for ln in lines], "rows": store.rows}
    changed = ISS1[:30] + "9" + ISS1[31:]
    return {
        "text": text,
        "parsed": [{"norad": n, "name": v[0], "l1": v[1], "l2": v[2], "epoch": d.tle_epoch(v[1])} for n, v in sorted(tl.items())],
        "checksums": [[ln, d.tle_checksum(ln)] for ln in (ISS1, ISS2, SO1, "1 25544U 98067A   26300.51782528 -.00016717 -0000-0  30144-3 0  99", "")],
        "catalogs": [[ln, d.tle_catalog(ln)] for ln in (ISS1, SO2, "1 A0001U" + " " * 61, "1 J0001U" + " " * 61, "1 P0001U" + " " * 61,
                                                     "1 Z9999U" + " " * 61)],
        "bad_texts": [changed + "\n" + ISS2 + "\n", ISS1 + "\n" + SO2 + "\n", ISS1[:68] + "\n" + ISS2 + "\n", ISS1 + "\n",
                      ISS1[:68] + "x\n" + ISS2 + "\n", ""],
        "store": rows,
        "runs": run,
    }


def main():
    out = json.dumps(build(), indent=1, sort_keys=True) + "\n"
    path = VECTORS
    if sys.argv[1:] == ["--check"]:
        if not os.path.exists(path) or open(path).read() != out:
            print("tests/data/sat_update_vectors.json is not what tools/rfoundry_debug.py gives now: run tools/sat_vectors.py", file=sys.stderr)
            return 1
        print("sat_update_vectors.json: current")
        return 0
    open(path, "w").write(out)
    print("wrote", path)
    return 0


sys.exit(main())
