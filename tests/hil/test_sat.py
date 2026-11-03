# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
"""Satellite tracking on the radio (DL_CMD_SAT, docs/spec/debuglink.md 3.24, docs/spec/satellite.md): the orbit model,
look angles and passes worked out on the radio, against the reference values the host test uses (tests/host/sat_ref.h,
from python-sgp4 and Skyfield), and the model's time a run on the radio's CPU. Radio only, receive only: nothing is
tuned or keyed. Loading a TLE and setting the observer change only the tracker's RAM; the clock is set too and an NMEA
sentence puts it back. The store test writes one record to the satellite area, for a made-up satellite (catalog 100001)
no real one uses, and deletes it again."""
import pathlib
import re

import pytest

from rfoundry_hil import LONG, skip

REF = pathlib.Path(__file__).resolve().parent.parent / "host" / "sat_ref.h"
# One update a second is the requirement for live tracking (docs/dm32-satellite-plan.md): a run must be far under it
RUN_US_MAX = 100000


def reference():
    """The element sets, the observer, the looks and the passes of sat_ref.h"""
    text = REF.read_text()
    tles = re.findall(r'\{ "([^"]+)", "(1 [^"]+)", "(2 [^"]+)" \}', text)
    obs = [float(x) for x in re.search(r"ref_obs\[3\] = \{ ([^}]+) \}", text).group(1).split(",")]

    def rows(name):
        body = re.search(name + r"\[\] = \{(.*?)\n\};", text, re.S).group(1)
        return [[float(x) for x in m.split(",")] for m in re.findall(r"\{ ([^{}\n]+) \},", body)]
    return tles, obs, rows("ref_looks"), rows("ref_passes")


@pytest.fixture
def sat(radio):
    if "SAT" not in radio.features():
        skip("not_built", "built without CONFIG_SAT")
    tles, obs, looks, passes = reference()
    tle, sgp4, norad = radio.link.sat_load(tles[0][1], tles[0][2])
    assert (tle, sgp4) == (0, 0), f"the reference ISS element set didn't load: tle {tle}, sgp4 {sgp4}"
    radio.link.sat_observer(*obs)
    return radio.link, looks, passes


@pytest.mark.feature("sat")
def test_sat_look_matches_reference(sat):
    link, looks, _ = sat
    worst_az = worst_el = worst_rr = 0.0
    n = 0
    for tle, unix_s, az, el, rng, rr in looks[::6]:
        if tle != 0:
            continue
        got = link.sat_look(unix_s)
        daz = abs(got["az"] - az)
        daz = min(daz, 360 - daz)
        if 0 < el < 85:
            worst_az = max(worst_az, daz)
        worst_el = max(worst_el, abs(got["el"] - el))
        worst_rr = max(worst_rr, abs(got["range_rate"] - rr))
        assert abs(got["range_km"] - rng) < 0.1, f"range at {unix_s}: {got['range_km']} km, reference {rng}"
        n += 1
    print(f"\n# {n} looks: worst az {worst_az:.3f} deg, el {worst_el:.3f} deg, range rate {worst_rr * 1000:.2f} m/s")
    assert n >= 20
    # The reply's 0.01 degree and the host test's agreement (0.004 degree, 0.3 m/s)
    assert worst_az < 0.02 and worst_el < 0.02 and worst_rr < 0.001


@pytest.mark.feature("sat")
def test_sat_passes_match_reference(sat):
    link, _, passes = sat
    n = 0
    for tle, start, aos, tca, los, max_el in passes:
        if tle != 0:
            continue
        p = link.sat_pass(int(start), 3600, 0)
        assert p["found"], f"no pass found from {int(start)}"
        assert abs(p["aos"] - aos) < 2 and abs(p["los"] - los) < 2, f"pass {p} against rise {aos}, set {los}"
        assert abs(p["tca"] - tca) < 3 and abs(p["max_el"] - max_el) < 0.05, f"pass {p}: peak {tca}, {max_el} deg"
        n += 1
        if n == 4:
            break
    assert n == 4


@pytest.mark.feature("sat")
def test_sat_model_speed(sat, record_property):
    """The time a run of the model takes on the radio's CPU (docs/spec/satellite.md 7): what decides how often the
    radio can update a live pass and how long a pass list takes"""
    link, _, passes = sat
    runs, us = link.sat_bench(1000)
    assert runs > 0
    per_run = us / runs
    p = link.sat_pass(int(passes[0][1]), 3600, 0)
    record_property("sat_us_per_run", round(per_run, 1))
    record_property("sat_pass_us", p["us"])
    print(f"\n# {runs} runs in {us} us: {per_run:.1f} us a run; a pass search: {p['runs']} runs in {p['us']} us; "
          f"a day of passes (about 1,600 runs) would take about {per_run * 1600 / 1e6:.2f} s")
    assert per_run < RUN_US_MAX, f"{per_run:.0f} us a run: too slow for one update a second"
    v = link.view(["sat"])
    assert int(v["sat.bench_runs"]) == runs


@pytest.mark.feature("sat")
def test_sat_store_round_trip(radio):
    """A satellite saved to the radio's flash, listed, recalled into the tracker and deleted (satellite.md 6.3)"""
    if "SAT" not in radio.features():
        skip("not_built", "built without CONFIG_SAT")
    link = radio.link
    v = link.view(["sat"])
    if v.get("sat.store") == "none":
        skip("not_built", "this radio's flash map has no satellite area (hal_flash_map_t.sat_addr)")
    if v.get("sat.store") == "foreign":
        skip("state", "the satellite area holds someone else's bytes: back up the data flash, then "
                      "rfoundry_debug.py sat clear")
    tles, obs, _, _ = reference()
    name, l1, l2 = next(t for t in tles if t[1][2:7] == "A0001")
    assert link.sat_load(l1, l2) == (0, 0, 100001)
    link.sat_observer(*obs)
    stored = int(v["sat.stored"])
    link.sat_save(437000000, 145900000, 885, name="HIL-TEST")
    try:
        found = [(slot, r) for slot, r in link.sat_list() if r["norad"] == 100001]
        assert len(found) == 1, f"the saved record isn't listed once: {found}"
        slot, r = found[0]
        assert r["name"] == "HIL-TEST" and r["up_hz"] == 437000000 and r["down_hz"] == 145900000
        assert r["up_tone"] == 885 and r["elements"]
        before = link.sat_look(1793000000.0)
        # Something else in the tracker, then the stored one back: the same answer
        assert link.sat_load(tles[0][1], tles[0][2])[2] == 25544
        assert link.sat_recall(slot) == (0, 100001)
        after = link.sat_look(1793000000.0)
        assert (after["az"], after["el"], after["range_km"]) == (before["az"], before["el"], before["range_km"])
        assert after["tx_hz"] and after["rx_hz"], "the stored profile's frequencies didn't come back"
        assert int(link.view(["sat"])["sat.stored"]) == stored + 1
    finally:
        link.sat_delete(100001)
    assert int(link.view(["sat"])["sat.stored"]) == stored


@pytest.mark.feature("sat")
def test_sat_screens_open(radio):
    """Tools > Satellites (OK 6 0) lists what the store holds, and BACK leaves it (docs/spec/ui.md 2.6). Keys only"""
    if "SAT" not in radio.features():
        skip("not_built", "built without CONFIG_SAT")
    radio.ensure_awake()
    if radio.screen() != "home":
        radio.press("back", LONG)
    screen = lambda name: radio.wait_for(lambda v: v.get("ui.screen") == name, sections=("link",), what=name)
    screen("home")
    stored = int(radio.link.view(["sat"]).get("sat.stored", 0))
    try:
        radio.press("ok")
        screen("hub")
        radio.press("6")
        screen("tools")
        radio.press("0")
        v = screen("satellites")
        assert int(v.get("ui.count", -1)) == min(stored, 32), f"{v.get('ui.count')} rows for {stored} stored"
        radio.press("back")
        screen("tools")
    finally:
        radio.press("back", LONG)
