<!-- SPDX-License-Identifier: MIT -->

# The satellite element set page

A web page that keeps a radio's satellite element sets (TLEs) current, sets its clock from the phone or computer and
reads its diagnostics, over the programming cable. It lives in its own repository,
[MabeCr/sat-tle-tools](https://github.com/MabeCr/sat-tle-tools) (plain files, no build step, MIT licensed), published
with GitHub Pages at <https://mabecr.github.io/sat-tle-tools/>. It is not part of the RadioFoundry web site, and this
repository holds no copy of it: one source, so a change is made once.

## What it uses of the firmware

Only what the radio already offers over the [debug link](debug-link.md): `VERSION`, the `sat`, `tasks` and `crash` view
sections, and the `SAT` command ([debuglink.md](../spec/debuglink.md) 3.24): TLE, LOAD, SAVE, STORED, DELETE, TIME,
BENCH, and the retune test (RETUNE_TEST, RETUNE_RESULT: [the plan](satellite-doppler-plan.md)). Nothing in the firmware is there for the page alone. The update loop is the one in `tools/rfoundry_debug.py`
(`sat_update`), rule for rule: a set is used only if both lines are 69 characters with correct check digits and name the
same catalog number, and replaces a stored one only if its epoch is later (with a one-second tolerance, because the radio
reports the epoch rounded); a stored satellite keeps its profile (frequencies, tones, arming, name).

## The shared test vectors

The two implementations (Python here, JavaScript there) must agree. `tools/sat_vectors.py` works the outcomes out with the
Python tool and writes `tests/data/sat_update_vectors.json`: parsed sets, check digits, catalog numbers (Alpha-5 too),
bad inputs, and the result of an update and a dry run against a fake store. `tools/ci.sh` fails if the file differs from
what the Python tool gives now. The page's repository keeps a copy as `test/vectors.json`, its Node tests must agree
with it, and its CI compares that copy with this repository's.

To change the rules: change `tools/rfoundry_debug.py`, run `python3 tools/sat_vectors.py`, commit the new JSON, copy it
to the page's `test/vectors.json`, and make its tests pass.

## Connecting from a phone

Web Serial on Android lists only USB devices of the standard serial class (and Bluetooth serial ports). A programming
cable with an FTDI chip isn't one, so the page drives it directly over WebUSB: the chip's vendor requests set 115200 baud,
8N1, no flow control and a short latency timer, and each bulk-in packet starts with two status bytes that are dropped.
On a computer the operating system's driver owns the cable and Web Serial is used.

## Not on the page

Anything that writes outside the satellite area: no firmware, no codeplug, no clear-all without a typed confirmation.
