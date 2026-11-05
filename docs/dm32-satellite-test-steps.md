<!-- SPDX-License-Identifier: MIT -->

# DM-32UV satellite build: test steps

For the first run of the satellite build on the radio. Nothing here transmits; leave the antenna on and PTT alone. Each
step says what you should see, and what to send me if you don't. Run the commands from the repository with the
programming cable plugged in and the radio on, Home showing.

**The DM-32UV has no USB port of its own, so always name the port.** Find it with `ls /dev/ttyUSB*` (usually
`/dev/ttyUSB0`) and set it once: `export RFOUNDRY_PORT=/dev/ttyUSB0`. If you get a permission error, join the `dialout`
group (`sudo usermod -aG dialout $USER`, then log out and in). Only one program may hold the cable at a time, so close
the web installer's tab first. The tool needs pyserial: `pip install pyserial`.

**On WSL (Ubuntu inside Windows):** WSL can't see the cable, which is plugged into Windows. Run the tool with Windows'
Python instead: install Python from python.org (tick "Add python.exe to PATH"), `pip install pyserial` in PowerShell, find
the cable's port in Device Manager > Ports (COM & LPT, say `COM3`), then from PowerShell, in the repository's folder
(`cd \\wsl.localhost\Ubuntu\home\<you>\<path>\RadioFoundry-sat-firmware`; if PowerShell won't use that folder, copy the
`tools` folder to `C:\rf\tools`):

```powershell
$env:RFOUNDRY_PORT = "COM3"
python tools\rfoundry_debug.py version
```

Use `python tools\rfoundry_debug.py` wherever the steps say `rfoundry_debug.py`. Step 12 (pytest) needs more setup on
Windows: skip it until the rest works.

`rfoundry_debug.py` below means `python3 tools/rfoundry_debug.py`, run from the repository folder (it is not an installed
command).

Keys from Home: **OK** opens the menu, then **6** is Tools and **7** is Settings.

## 1. It boots and is still the same radio

| Do | Expect |
|---|---|
| Power on | Home appears, backlight, keys and the knob work |
| Change channel, step a zone with Left / Right | works as before |
| Receive on an FM channel you know (a repeater, or scan) | audio plays, the status strip shows RX |
| Settings > Display > Theme | Night is still there and works, disabled rows are readable |
| OK, 6 (Tools) | a **0 Satellites** row at the bottom, the hint reads "0-9 open" |

If it doesn't boot to Home: take the battery off, update mode (PTT + SK1 at power-on), reinstall the standard release.

## 2. The debug link sees the satellite build

```bash
rfoundry_debug.py version
rfoundry_debug.py view --section sat
```

Expect `features=` to contain `SAT`, and `sat.built=1`, `sat.store=ours`, `sat.slots=32`, `sat.stored=0`.

- `sat.store=foreign`: the area holds someone else's bytes. Back up the data flash first, then `rfoundry_debug.py sat clear`.
- `sat.store=none`: the board has no satellite area: send me the output.

## 3. Memory: the number the plan is waiting for

```bash
rfoundry_debug.py view --section tasks
```

Write down `tasks.heap_min_free` (must stay above **2048**; upstream saw 8,664) and `tasks.min_free` with
`tasks.min_task`. There should be a `sat` task. Use the radio for five minutes, open Tools, Settings, Diagnostics, then
run it again. Repeat at the end of testing.

## 4. Load satellites

With the computer online:

```bash
rfoundry_debug.py sat update --add 25544 --add 27607 --add 43017
rfoundry_debug.py sat list
rfoundry_debug.py sat update
```

Expect the first command to read some element sets and store the ISS, SO-50 and AO-91 ("stored: new set N days
old"). `sat list` shows the three with their frequencies (the ISS 145.9900 / 437.8000, SO-50 with arm 74.4, AO-91). The
last command says "already current" for each.

Offline: download <https://celestrak.org/NORAD/elements/gp.php?GROUP=amateur&FORMAT=tle> on any computer, save it as
`amateur.txt` and add `--file amateur.txt` to both `update` commands. If the download address itself fails, send me the
message: it's the one thing I couldn't test.

## 5. How fast the orbit maths runs

```bash
rfoundry_debug.py sat recall 0      # tracks the first stored satellite
rfoundry_debug.py sat bench 200
```

Write down the line "N runs in X us: Y us a run". Expected: a few
milliseconds. Under 100 ms (100000 us) is fine; far above that, tell me.

## 6. The radio's time and position

The screens need both. They aren't set yet.

- **Time:** a GPS fix gives it. Indoors: `rfoundry_debug.py sat time` copies this computer's clock to the radio.
- **Position:** OK, 6, 8 (APRS), then 9 (Position). Row 1 **Source**: pick **Fixed**. Row 2 **Latitude**: OK, type the
  digits (`3 3 7 4 9 0` is 33.7490), OK. Row 3 **Longitude**: OK, digits (`0 8 4 3 8 8 0` is 84.3880), press **Down**
  to make it West, OK. Use your real location. Or go outdoors for a GPS fix instead of both.

Settings > Location (OK, 7, 5) > **Time zone**: OK, then Up / Down move in 15 minute steps, OK to keep. Set your own
offset ("UTC-6" for Central standard time).

## 7. The screens

OK, 6, then **0** (Satellites).

| Look for | Expect |
|---|---|
| The title | "Satellites" and the local time, matching your clock |
| The line under it | "Above 10°, 24 h, UTC-6" (your offset) |
| The rows | ISS, SO-50, AO-91: first "…", then each fills in with an arrow, a rise time and a peak elevation; a satellite overhead now reads "up now N°" in green |
| How long they take | note the time until all three have filled in (about a minute is fine) |
| OK on one | its passes: "TX … RX …" and its tones at the top, then the next 24 hours of passes with times, peaks, an arrow and "NW→SE 13m"; a "Tomorrow" line between the days |
| Back, Back | back to Tools, where the 0 row shows the next pass ("ISS 08:32") |

**Check the predictions against something else.** Use any satellite tracker for your location (an app, N2YO, Heavens
Above, gpredict): the ISS's next pass rise time and peak elevation should agree within about a minute and a degree or
two. This is the accuracy test: the host tests only compared the maths with other software, never with the radio's
clock.

If a row stays "…" for several minutes, the `sat` task may not be running: send me `view --section tasks` and
`view --section sat`. If the screen says "Waiting for the time" or "No position", step 6 isn't done.
"Not enough memory to work out passes" means the task couldn't start: send me the heap line from step 3.

## 8. The settings

| Do | Expect |
|---|---|
| Settings > Location > Lowest pass: set 40° | the lists lose their low passes within a few seconds; a satellite with none says "no pass" |
| Set it back to 10° | the passes return |
| Change Time zone by 1 hour | every time on the screens moves by an hour |
| Power the radio off and on | both settings are still as set |

## 9. The debug link's view of one satellite

```bash
rfoundry_debug.py sat recall 0
rfoundry_debug.py sat time
rfoundry_debug.py sat observer 33.749 -84.388 300     # your latitude, longitude, height in meters
rfoundry_debug.py sat look
rfoundry_debug.py sat pass
```

`sat look` prints azimuth, elevation, distance and the Doppler-corrected receive and transmit frequencies at this
moment. `sat pass` prints the next pass. Compare them with the tracker from step 7.

## 10. Outdoors with a GPS (optional)

Settings > Location > GPS **On**, Tools > Position, wait for a 3D Fix, and set Tools > APRS > Position > Source back to
**Auto**. With a fix, the time and position come by themselves: "Waiting for the time" disappears from the list without
`sat time`.

## 11. Soak

Leave the Satellites list on screen for 10 minutes. Then:

```bash
rfoundry_debug.py view --section tasks     # heap_min_free still above 2048; tasks.min_free for each task
rfoundry_debug.py crash show               # no crash record
```

Settings > Advanced > Crash log should say there is nothing. The radio must not have restarted.

## 12. The automated tests

Release the cable (close anything else using it), then:

```bash
python3 -m pytest tests/hil/test_task_stacks.py tests/hil/test_sat.py -v -rs --radio-port /dev/ttyUSB0
```

`test_sat.py` compares the radio's look angles and passes with the reference values, stores, recalls and deletes a
made-up satellite (catalog 100001, never a real one), opens Tools > Satellites, and prints the time per model run.
Expect all to pass; a skip is fine if it says why.

## 13. Clean up (optional)

`rfoundry_debug.py sat delete 25544` removes a satellite; `sat clear` empties the store.

## What to send me

- The **`tasks.heap_min_free`** and **`tasks.min_free`** lines from steps 3 and 11.
- The model's **time per run** (step 5) and **how long the list took to fill** (step 7).
- Whether the **predicted passes matched** the tracker (step 7), and by how much.
- Anything that didn't match "Expect", with what the radio showed. A screenshot helps:
  `rfoundry_debug.py screenshot -o sat.png`.
