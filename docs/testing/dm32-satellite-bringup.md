<!-- SPDX-License-Identifier: MIT -->

# DM-32UV satellite build: bring-up

For the satellite configuration of the DM-32UV (`CONFIG_SAT`, [satellite.md](../spec/satellite.md)): the build and its
size checks, the install, and the steps to try on the radio. It follows [dm32-bringup.md](dm32-bringup.md), which holds the
hardware steps and the recovery path. Nothing here transmits: leave the antenna on and PTT alone.

What a radio is claimed to support comes from recorded runs (`tools/bringup.py`, `tests/hil`), not from this page.
Section 5 is a dated log of what was seen by hand, so a reader knows what is still untested. The plan and its decisions:
[dm32-satellite-plan.md](../dm32-satellite-plan.md).

## 1. Build and measure (no radio)

The satellite build is the radio's own defaults (English only, no vibration alerts) plus satellite tracking, then the cuts
in `SATELLITE_CUTS` of `boards/dm32/config.mk` that make room for its RAM. CI builds and stack-checks it on every push, and
publishes it ([section 2](#2-install)). The board's *maximal* build (every switch on, Italian included) is a coverage build
that CI also compiles: never flash it.

| Step | Command | Expect |
|---|---|---|
| Compiler | `tools/docker/csky-toolchain/run.sh csky-elf-gcc --version` (or `tools/build-csky-toolchain.sh` once, about 45 minutes, then it installs into `~/toolchains/csky-elf`) | GCC 14 |
| Build the baseline | `tools/docker/csky-toolchain/run.sh make BOARD=dm32 BUILD=build/dm32-base` | 587 KB of flash, **16 bytes** of RAM free (upstream's number; if yours is several KB off, the compiler or checkout differs: stop and say so) |
| Build the satellite configuration | `tools/docker/csky-toolchain/run.sh tools/satellite.sh dm32` | no warnings; the numbers in section 5 |
| Stack check | `tools/docker/csky-toolchain/run.sh tools/satellite.sh dm32 stack-check` | `every task fits`; note the least free of `ui`, `sat` and `host` |
| Host tests | `make -C tests/host run-sat` | all four satellite tests print `passed` |

Without Docker, run the same commands without the `run.sh` prefix. The linker prints `FLASH:` and `RAM:` lines: the
satellite build must leave a few KB of RAM free (the estimate was 20 KB, the measurement 23.7 KB). If a task is shown
`OVER`, give that task more stack out of the free RAM (`UI_TASK_STACK_WORDS` in `core/ui/ui.c`, or the satellite
task's `SAT_TASK_STACK` in `core/app/sat_task.c`) and send the stack-check output.

## 2. Install

1. **Back up both flashes first** ([dm32-bringup.md](dm32-bringup.md) section 0): the 1 MB program flash and the 16 MB
   data flash. The data flash holds this radio's factory calibration; keep two copies, and never put a backup in a git tree.
2. Get the image, one of two ways:
   - **From a phone, with no computer.** GitHub Actions builds the satellite configuration on every push to `main` and
     publishes it as a pre-release: the repository's Releases page, the newest `satellite-<commit>`. Its notes give the
     commit, the `FLASH:` and `RAM:` lines, the stack check and the SHA-256. Download `rfoundry_dm32_satellite.bin` in
     the phone's browser.
   - **From a computer:** `build/dm32-satellite/rfoundry_dm32.bin` (section 1), or the `_codec.bin` that `make` writes when
     `codecs/` holds the stock file.

   The cloud image has **no codec**: the installer keeps the one already in a RadioFoundry radio and says so before it
   writes. Give the file to the RadioFoundry web installer's own-firmware upload (it works in a phone's browser), or from
   a computer run `rfoundry flash <file> --board dm32 --dry-run` and then without `--dry-run`. Update mode is **PTT + SK1**
   held while turning the radio on. Never publish an image that holds the codec.
3. It boots to Home, the keys and knob work, FM receives. If it doesn't boot: update mode always works (the bootloader is
   never written): install the standard release again.

## 3. Connect

The DM-32UV has no USB port of its own: the debug link is on the programming cable (a USB-serial adapter), so always name
the port, or use the browser page (below). Only one program may hold the cable at a time.

- **Computer:** `python3 tools/rfoundry_debug.py --port <port> version`. `rfoundry_debug.py` is not an installed command;
  `<port>` is `/dev/ttyUSB0` on Linux, `COM3` and the like on Windows (Device Manager > Ports). On WSL the cable is on the
  Windows side: run the tool with Windows' Python (`pip install pyserial`, then `python tools\rfoundry_debug.py ...` from
  PowerShell, with `$env:RFOUNDRY_PORT = "COM3"`).
- **Phone or computer, in a browser:** the [satellite element set page](../dev/sat-tle-web-tool.md): connect, set the
  clock, update the element sets, read the diagnostics. On Android it talks to an FTDI cable over WebUSB.

`version` must list `SAT` in `features=`; `view --section sat` shows `sat.built=1`, `sat.store=ours`, `sat.slots=32`.
`sat.store=foreign` means the area holds someone else's bytes: back up the data flash, then `sat clear`.

## 4. Steps on the radio

Keys from Home: **OK** opens the menu, **6** is Tools, **7** is Settings.

| # | Do | Expect |
|---|---|---|
| 1 | Boot, change channels, receive on a known FM channel | as before the satellite build; Settings > Display > Theme still has Night |
| 2 | `view --section tasks` | `tasks.heap_min_free` above 2,048 (upstream saw 8,664 without the satellite task); there is a `sat` task. Write down `heap_min_free` and `min_free` with `min_task`, and again after the soak |
| 3 | Tools: the last row is **0 Satellites** | hint "0-9 open" |
| 4 | Set the clock: the browser page's *Set the radio's clock* button, or Settings > Location > **Set time** (type the UTC time, **#** between fields, OK as the minute starts), or a GPS fix, or `sat time` (RAM only) | Tools > Satellites stops saying "Waiting for the time" |
| 5 | Settings > Location > **Time zone** (your offset), **Lowest pass** (try 40, then back to 10) | the lists change within seconds; both survive a power cycle |
| 6 | Position: Tools > APRS > Position > Source **Fixed**, then latitude and longitude, or a GPS fix outdoors | "No position" goes away |
| 7 | Load element sets with the browser page (Download, Paste or File), or `sat update --add 25544 --add 27607 --add 43017` | the stored list shows the satellites with their ages |
| 8 | Tools > Satellites (OK, 6, 0) | each row fills in (an arrow, a rise time and a peak; "up now" in green); about a minute is fine; compare the next pass of the ISS with any tracker (a minute and a degree or two) |
| 9 | OK on a satellite, then OK on a pass | **Tracking**: the sky plot with the path, the arrow beside the path's middle, elevation, azimuth, AOS/LOS, RX and TX with Doppler. Hold OK on a list row jumps straight to its next pass |
| 10 | Leave Tracking on screen 15 minutes or more | the radio does not restart; then `crash show` has no new record and `view --section tasks` shows `heap_min_free` unchanged |
| 11 | **Power-cycle** the radio, open Tools > Satellites | the time is still there within a second or two: the battery-backed clock works. "Waiting for the time" means the clock's read or write failed |
| 12 | GPS outdoors (Settings > Location > GPS On) | the top bar shows a hollow square while searching, a solid one with a fix; the time and position come by themselves |
| 13 | `sat look`, `sat pass` (after `sat recall 0`, `sat time`, `sat observer LAT LON ALT`) | compare with the tracker of step 8 |
| 14 | `python3 -m pytest tests/hil/test_task_stacks.py tests/hil/test_sat.py -v -rs --radio-port <port>` | all pass; a skip says why. Needs the cable to itself, and more setup on Windows |
| 15 | The retune test (receive only), from the [TLE page](../dev/sat-tle-web-tool.md)'s *Retune test* section, or `sat retune-test 162.400 2 12 150` | On an FM channel or the VFO, with a steady FM carrier: a NOAA weather channel (162.400 to 162.550 MHz; the page's frequency is yours to set). A table of how long each retune took to settle and whether the signal dipped; the radio goes back to its channel. Listen while it runs: it clicks at each retune, and whether the audio drops is what to report. Try steps of 1, 2, 5 and 10 kHz and both widths. Send the table and what you heard |

Clean up when done (optional): `sat delete 25544` removes a satellite, `sat clear` empties the store.

If a row stays "…" for several minutes the `sat` task may not be running: send `view --section tasks` and
`view --section sat`. "Waiting for the time" or "No position" means step 4 or 6 isn't done. "Not enough memory to work
out passes" means the task couldn't start: send the heap line from step 2.

## 5. What was seen on the radio

A log by hand, newest last. Nothing here is a support claim.

| Date | What | Result |
|---|---|---|
| 2026-11-05 | The default image, built with the project's C-SKY Docker image | 587,392 B flash, 211,440 B RAM, **16 B free** |
| 2026-11-05 | The satellite configuration (typed by hand with the same cuts, `LANG_IT` and `VIBRATE` off) | 454,232 B flash (50%), 187,528 B RAM, **23,928 B free**; stack check: every task fits |
| 2026-11-05 | The same plus the Tracking screen | 457,796 B flash, 187,688 B RAM, 23,768 B free; stack check: `ui` 264 B, `sat` 152 B, `host` 924 B free |
| 2026-11-06 | The first cloud image, built from the maximal configuration | built clean in CI: 506,700 B flash, 187,720 B RAM (23.7 KB free), stack check fits (`sat` 116 B least free). **It came up in Italian**: the maximal build turns the Italian language on as the default. Replaced by the satellite build; do not flash `satellite-797b615` |
| 2026-11-05 | Flashed over the standard release; `version` and `view --section sat` | `SAT` in features; store ours, 32 slots |
| 2026-11-05 | `sat bench` | 3.76 ms a run (67 runs in 251.7 ms) |
| 2026-11-05 | Tasks after use | `heap_min_free` 15,416; least free stack `sat` 272 B on the radio |
| 2026-11-05 | The list and passes against an independent tracker | the ISS agreed; three satellites filled in 6 to 8 seconds |
| 2026-11-05 | `sat update` from the computer | the download timed out on Windows' Python (use `--file`); the browser page's download is untested |
| 2026-11-05 | Tracking on the radio | displayed and plausible; the arrow was in the wrong place (fixed) |
| 2026-11-05 | A Tracking soak of a few minutes | **the UI task hung 3 s (watchdog, `crash.kind=5`, `crash.task=ui`) after 681 s**: a reader waited on a write it had preempted. Fixed (readers give up after 50 tries); not yet retested |
| 2026-11-05 | The browser page: connect (FT231X cable, WebUSB, Android phone), set the radio's clock, stored list, diagnostics | works |
| 2026-11-11 | A real pass with the automatic mode (`*`) | the receiver followed the Doppler in steps about every 30 s; **no click heard** at the retunes. By ear, one pass; the squelch state wasn't recorded |
| | The battery-backed clock across a power cycle, Set time, the GPS mark, a 15 minute Tracking soak on the fixed build | **untested** (steps 10 to 12) |

## 6. What to send back

- `tasks.heap_min_free`, `tasks.min_free` and `tasks.min_task` after the soak.
- The `FLASH:` and `RAM:` lines and the stack-check summary of the build you flashed.
- Anything that didn't match "Expect", with what the radio showed; a screenshot helps:
  `rfoundry_debug.py screenshot -o sat.png`.
- If the radio restarts: `crash show` before anything else, and a photo of Settings > Advanced > Crash log.
