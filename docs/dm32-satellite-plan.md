<!-- SPDX-License-Identifier: MIT -->

# DM-32UV satellite firmware plan

This fork of RadioFoundry adds satellite operation to the DM-32UV: on-radio orbit prediction and automatic Doppler
correction, with risk-averse work first. This page holds the plan, the decisions made so far and what is still open.
It belongs to this fork, not to upstream.

Status as of 2026-11-05: the satellite feature is built and running on the radio: the orbit library, the stored
satellites, Tools > Satellites with passes and Tracking, a battery-backed clock with a Set time screen, a GPS mark in the
status strip, and a browser page that updates the element sets from a phone ([dev/sat-tle-web-tool.md](dev/sat-tle-web-tool.md)).
Still to build: Doppler tuning of the radio, the SO-50 arming countdown and key, and pass alarms. What was seen on the
radio, and what is still untested, is the log in [testing/dm32-satellite-bringup.md](testing/dm32-satellite-bringup.md)
section 5. The work is on branch `claude/peaceful-ride-m0ie2u` and in
[pull request 3](https://github.com/MabeCr/RadioFoundry-sat-firmware/pull/3).

Feasibility notes come from public documents unless they name a file in this repository. Anything not tested on the
radio is untested, however likely it looks.

## Repository and builds

| Topic | Decision |
|---|---|
| Where the work lives | This repository: the whole RadioFoundry tree plus our commits. RadioFoundry has no plugin or overlay mechanism, so features edit the source directly |
| `origin` | [MabeCr/RadioFoundry-sat-firmware](https://github.com/MabeCr/RadioFoundry-sat-firmware), a GitHub fork. Branches and pull requests go here |
| `upstream` | [RadioFoundry/RadioFoundry](https://github.com/RadioFoundry/RadioFoundry), fetch only. Merge from it to stay current. Keep the other radios' code, or every merge conflicts |
| Pull requests | Open them with base `main` of this fork. GitHub's own links can default to upstream as the base |
| Firmware images | Never committed. An image can contain the merged AMBE codec, so publish only images without it |
| The website's custom builder | Its documentation says the build service builds release tags, and nothing connects it to this fork. Its images have none of this fork's work, so build from this repository |

## Build configuration

The configuration chosen for satellite use on the DM-32UV is the radio's own defaults plus `CONFIG_SAT=y` and the cuts in
`SATELLITE_CUTS` of `boards/dm32/config.mk` (the fourteen below and their sub-features). `tools/satellite.sh` builds it, and
CI builds and stack-checks it on every push and publishes the image (`.github/workflows/satellite-image.yml`), so the RAM
budget is checked there, not only on a laptop:

```
tools/docker/csky-toolchain/run.sh tools/satellite.sh dm32                # build/dm32-satellite/rfoundry_dm32.bin
tools/docker/csky-toolchain/run.sh tools/satellite.sh dm32 stack-check
```

This is the configuration the first measurements (below) were made with, typed by hand. The board's *maximal* build,
every switch on with the same cuts, is a separate coverage build for CI: it also turns on the Italian language as the
default, so it is never flashed.

| Switch | State | Why |
|---|---|---|
| `SFR` | Off | Not related to satellites, and experimental. `SFR_DUALPATH_RX` and `SFR_CWID` follow it |
| `DMR_PRIVACY`, `DMR_TMS`, `DMR_ARS`, `DMR_LRRP`, `DMR_CONTROL_STATION`, `SNMP`, `HOTSPOT_SPLIT` | Off | Not needed for this use |
| `LANG_ES` | Off | English only. `LANG_IT` is already off by default |
| `NET_USRP`, `HOTSPOT`, `ZONE_SHARE`, `NET_HTTP`, `DMR_ROAMING` | Off | Not needed, and the five largest RAM costs left after dual watch ([Further cuts](#further-cuts)). `HOTSPOT_SPLIT` and MMDVM SFR go with `HOTSPOT`; `NET_WEBCTL` and `NET_HTTP_GZIP` with `NET_HTTP`. `rigctld`, KISS over TCP and APRS-IS stay |
| `VOX`, `VIBRATE` | Off by default | Not written for this radio. The vibration motor's drive pin isn't known ([hardware-dm32.md](spec/hardware-dm32.md) 6.6) |
| `USB`, `USB_NETWORK`, `NAND_STORAGE` | Unsupported | The radio has no USB data port and no SPI NAND |
| `DMR` | **Keep on** | `NET_PPP` needs it on this platform (`platform/hrc7000/config.mk`), and the IP gateway and `rigctld` ride that link |
| `DUAL_WATCH` | **Keep on** | Two channels on Home, for everyday use outside satellite passes. The largest RAM cost left (5.3 KB, [Further cuts](#further-cuts)), kept by choice |
| `CRASH_LOG`, `DEBUGLINK`, `FAULT_TRACE`, `WATCHDOG`, `STUB_ENTRY` | **Keep on** | They are what diagnoses and recovers a first build on a new radio |
| `VFO`, `SUBTONE`, `APRS`, `GPS`, `GPS_SKY`, `UI_COLOR` | **Keep on** | Satellite operation and the Night theme use them. `GPS_SKY` needs both `GPS` and `APRS`. `APRS` also carries the NMEA that sets the clock |
| `SAT` | **On** | The satellite library. Off by default in the tree until the radio uses it |

Operating note: leave Settings > Power > Battery saver off during a pass. After 10 s idle the receiver listens only in
100 ms windows.

## Hardware constraints

Facts with a section number are in [hardware-dm32.md](spec/hardware-dm32.md). The rest come from public documents and
are to be confirmed on the radio.

| Part | What we know | Consequence |
|---|---|---|
| CPU | C-SKY CK803S in the HR_C7000, 168 MHz. Built with no FPU (`platform/hrc7000/platform.mk`: `-msoft-float`) | SGP4's doubles run in software. Measure the cost |
| RAM | 288 KB, 211,456 B of it below the codec's window. **The limit on this radio**: the default build leaves about 0.5 KB (`boards/dm32/config.mk`) | Every new feature must find its RAM. See [RAM budget](#ram-budget) |
| Program flash | 1 MB, bootloader in the first 48 KB, 880 KB of code space before the codec slot | Less tight than RAM |
| Data flash | 16 MB SPI. The port writes nothing below 0x700000, where the stock codeplug and calibration live | Stored satellites use 0x71B000 (one sector), in the gap between the settings journal and the open-firmware flash image. The stock firmware's use of 0x6DC000 and up is undocumented: if the store reports `foreign`, back up and `rfoundry_debug.py sat clear` |
| RF chip | FD6818, frequency in 10 Hz units (8). FM, AM, FSK, DMR pass-through. No SSB or CW receive | Smooth Doppler steps. No real CW receive |
| Signal readings | RSSI in 0.5 dB steps, plus noise and SNR | Enough for a pass plot and a stepped sweep |
| Synthesizer | One, for receive and transmit (8) | No full duplex: no hearing the downlink while transmitting, and no measuring the uplink |
| Tone generators | Two, plus CTCSS/DCS and a sidetone output | Morse tone with an access tone is unverified |
| Display | 240x320 TFT drawn with a 16-color palette | UI designs fit 16 colors. Images need a new full-color path |
| GPS | ATGM336H on a UART at 9600. NMEA arrives on the radio; a fix outdoors is still to check ([dm32-bringup.md](testing/dm32-bringup.md) 12) | Time and position source once confirmed |
| Clock | RTC with coin-cell backup (3.3). No driver reads it yet ([feature-matrix.md](feature-matrix.md)) | Until one does, time comes from GPS (NMEA RMC), so nothing predicts before the first fix ([satellite.md](spec/satellite.md) 4.1). An RTC driver is a prerequisite for prediction indoors |
| Ports | K1 programming jack (serial). USB-C charges only | All computer and phone links use the K1 cable |
| Keys | Keypad, two side keys, PTT. PTT shares a wire with the serial line | A speaker-mic and a computer link can't be used together |

Board revisions V1.2 and V1.4 are documented. Boards from 2025 on may differ, and some units have a separate hardware
vocoder.

### RAM budget

RAM decides what fits, so each feature below must state its RAM cost. `tasks.heap_min_free` shows whether the RTOS heap
has room to shrink (`tests/hil/test_task_stacks.py`).

The C-SKY compiler isn't built yet, so these figures come from another radio's ARM Cortex-M4 build with the same core
code (LTO, 2026-11-03). Most of the RAM below is the core's own buffers, so the DM-32UV should be close, but confirm at
its first build.

| Configuration | Flash | RAM |
|---|---:|---:|
| That radio's default | 507,344 B | 178,680 B |
| The first nine switches off | 434,800 B | 168,528 B |
| The configuration above: those, the five further cuts, `SAT` on | 375,896 B | 149,064 B |
| Saved against the default | 131,448 B | 29,616 B |

That configuration builds with no warnings. Its static stack check finds the UI task 16 bytes over its limit (the
default build has 8 bytes to spare there): leaving features out changed what the link-time optimizer inlined into the
UI's settings code. The satellite code is not in that task. Two consequences, to check at the first DM-32UV build:

- Run `make BOARD=dm32 <the line above> stack-check`. If the UI task is over, give it more stack from the RAM freed
  here.
- The satellite work needs about 0.75 KB of stack wherever it runs. The UI task has none to spare, so give it that
  room or run the predictions elsewhere.

Satellite tracking as built so far (the library, stored satellites, the pass list screens and their task, the debug
link's SAT command): about 27 KB of flash, 2.8 KB of RAM and a 1.5 KB task stack on an ARM build
([satellite.md](spec/satellite.md) 7). Stored satellites cost no RAM beyond the list's 32 rows.

#### Does it fit? (measured, 2026-11-05)

Yes. Built with csky-elf GCC 14 (`tools/docker/csky-toolchain/run.sh`), the satellite configuration above links with
23,928 bytes of main RAM to spare. The linker refuses an image that doesn't fit, so this cannot fail on the radio.

| Build | Flash (880 KB) | Main RAM (211,456 B) | RAM free |
|---|---:|---:|---:|
| The standard DM-32UV image (satellites off) | 587,392 | 211,440 | 16 |
| The configuration above, `CONFIG_SAT=y` | 454,232 (50%) | 187,528 (88.7%) | **23,928** |
| The same with the Tracking screen (2026-11-05) | 457,796 (50.8%) | 187,688 (88.8%) | **23,768** |

The 14 switches turned off free 47 KB of RAM and 160 KB of flash; satellite tracking takes back about 2.8 KB of static
RAM, the 2 KB of extra heap for its task and 27 KB of flash. The estimate made before the measurement was 20 to 25 KB
free. The DM-32UV has one RAM region now (`boards/dm32/memory.ld`), so there is no separate `RAM_LO` line.

The first C-SKY build also found a bug: the standard image, built with satellites *off*, overflowed main RAM by 16
bytes, because the settings struct (six copies in RAM) and the UI's settings table had grown for the new settings.
They are now only built with `CONFIG_SAT`, and the standard image is 16 B free again, as upstream's. Any further
setting added without a switch has the same risk: build the standard DM-32UV image before pushing.

The stack check on the C-SKY build passes: every task fits, the UI task with 264 B to spare, the `sat` task 148 B
(its deepest path ends in a soft-float library function, which the check counts as a flat 64 B), the least free overall
the idle task's 132 B.

What remains to be seen on the radio:

- The heap: the satellite task's 1.6 KB stack comes from it. Measured on a DM-32UV after the whole radio-only HIL
  suite, the heap's least free was 8,664 bytes (upstream #210, 2026-10-27, with a 33 or 34 KB heap), so the task fits with
  room; still, built with satellites the heap is 2 KB larger, and the task starts only if 2 KB would still be spare
  after it; otherwise the radio runs on and Tools > Satellites says "Not enough memory to work out passes", rather
  than the heap running out and the radio restarting at every boot.
- The real stack use: `tasks.min_free` and `tests/hil/test_task_stacks.py` show the least stack each task ever had free.

The checks after installing, on the radio: `pytest tests/hil/test_task_stacks.py` (`tasks.heap_min_free` at least 2 KB)
and `tests/hil/test_sat.py` (the model's speed, the screens). Steps 1 and 2, the size and the stack check, are done.

#### Further cuts

Each switch turned off on top of the first nine switch-offs, one at a time. Decided: keep `APRS_RX` and `DUAL_WATCH`;
turn off `NET_USRP`, `HOTSPOT`, `ZONE_SHARE`, `NET_HTTP` and `DMR_ROAMING` (in the configuration above). The rest:
open. Children follow their parent (`NET_HTTP` includes `NET_WEBCTL`; `HOTSPOT` includes split duplex and MMDVM SFR).

| Switch | RAM | Flash | What goes |
|---|---:|---:|---|
| `APRS_RX` | 6,984 | 9,344 | APRS receive. Needed for ISS APRS and telemetry capture: keep |
| `DUAL_WATCH` | 5,304 | 19,432 | Two channels on Home. Useful outside satellite passes: keep |
| `NET_USRP` | 4,624 | 3,016 | AllStarLink / DVSwitch audio over the IP link: off |
| `HOTSPOT` | 4,224 | 13,128 | The MMDVM hotspot over the K1 cable: off |
| `ZONE_SHARE` | 3,928 | 12,504 | Sending and importing zones over DMR: off |
| `NET_HTTP` | 3,840 | 18,696 | The status web page and web control over PPP: off. `rigctld` stays |
| `DMR_ROAMING` | 3,136 | 12,068 | DMR repeater roaming: off |
| `TCP_CONNS=2` | about 1,500 | — | One TCP connection fewer (3 by default on this platform). With the web page gone, `rigctld`, KISS over TCP and the APRS-IS client share them; at 2, only two of those can be connected at once. 1 would leave one |
| `NET_WEBCTL` alone | 1,000 | 12,992 | Web control; the status page stays |
| `ZONE_EDIT` | 896 | 4,952 | The zone editor |
| `MDNS`, `TFTP` | 336, 176 | 6,480, 8,300 | Name lookup and codeplug transfer over the IP link |

The rest measured under 0.5 KB of RAM each: `NET_KISS`, `DMR_CONFIRMED`, `CRASH_REPORT`, `CPS_FAST`, `DMR_SMS`,
`DMR_EMERGENCY`, `NET_APRSIS`, `TASK_STACKS`, `DMR_SIGNALING`, `CONTACT_EDIT`, `IDDB_COMPACT`, `DTMF`,
`DMR_UDP_COMPRESS`.

## Clean room

[CLEANROOM.md](CLEANROOM.md) applies to everything here, and it is stricter than "don't copy".

- Don't open, read or search another firmware's source, or any GPL code, even as a "behavior reference". That includes
  the OpenGD77 family and the custom firmware for the Quansheng K5. Work from `docs/spec/`, datasheets, public standards
  and permissively licensed code.
- Those projects show a feature can exist: satellite screens and alarms, sweep spectrum views, on-radio CW. They are not
  a design source. The UI is designed from `docs/ux/` principles.
- Write SGP4 fresh from Vallado's published algorithm. Many SGP4 ports are GPL. Check the license of any port, such as
  csgp4, before opening it.

## Safety

Back up both flash chips before anything else. They hold this radio's own RX/TX calibration and keys, which no other
radio's backup can replace. The procedure is [dm32-bringup.md](testing/dm32-bringup.md) section 0.

- [x] Note the board revision, stock firmware version and vocoder variant when the radio arrives
- [x] Back up the 1 MB program flash and the 16 MB data flash, and store copies in two places
- [x] Confirm the stock firmware can be reflashed before installing anything else
- [x] Use an FTDI cable: CH340 cables can be unreliable ([installing](user/02-installing.md))
- [ ] Do all transmit testing into a dummy load first (nothing transmits yet)

Risk ladder, lowest first: simulator-only work, receive-only features, transmit features. Anything that changes keying,
power or deviation is checked on a spectrum analyzer before going on the air. Backups never go in this repository
([AGENTS.md](../AGENTS.md) rule 2).

## Build order

Steps 1-4 can start in the simulator. Step 5 is the first real-hardware milestone.

| Step | Work | Status |
|---|---|---|
| 1 | Set up: build, host tests, UI simulator | Done. The firmware builds with the C-SKY Docker image (`tools/docker/csky-toolchain/run.sh`) and in CI |
| 2 | UI redesign and Night theme | Night theme merged. Redesign not started |
| 3 | Satellite profiles with SO-50 auto-arm | Library done: built-in profiles and the arming state machine, host-tested. Profiles and TLEs are stored in data flash (one sector at 0x71B000, 32 satellites), saved and listed over the debug link (`rfoundry_debug.py sat save`, `list`). Not yet: editing on the radio; keying the burst through the radio core |
| 4 | On-radio SGP4 with a pass list, tested against a reference library on a PC | Done in the simulator: SGP4, look angles and passes agree with python-sgp4 and Skyfield (`tests/host/satTest.c`); Tools > Satellites lists the stored satellites' next passes and one satellite's 24 hours of passes, in local time with direction arrows, worked out by a task of its own; Settings > Location > Time zone and Lowest pass. Untested on the radio: `tests/hil/test_sat.py` measures the model's speed there |
| 5 | Radio arrives: back up, install, confirm revert to stock. Then `pytest tests/hil/test_sat.py`: the model's speed on this CPU | Done: installed over the standard release, the model costs 3.76 ms a run. `test_sat.py` itself has not been run on Windows |
| 6 | Receive-only auto Doppler, then transmit Doppler | Not started |
| 7 | Crystal calibration and drift display | Not started |
| 8 | Keypad TLE entry, then sound transfer | Not started |
| 9 | Pass signal plot and contact markers | Not started |
| 10 | Voice-recorded callsigns | Not started |
| 11 | ID timer | Not started |

Stretch goals, after the core works: satellite logbook and ADIF export, ISS APRS mode, CW and Morse, spectrum view,
SSTV receive, and possibly computer control.

Pass alarms, auto pass setup and the phone companion have no step yet. They depend on steps 3, 4 and 8.

## Core features

### Night theme

Done: Settings > Display > Theme > Night ([ui.md](spec/ui.md) 8).

| Decision | Choice |
|---|---|
| Colors | Black, pure reds. Dim green only for receive, dim amber only for warnings |
| Status bar | Always plain. Filled status bar and Highlight color are dimmed in Night |
| Editing a setting | Amber outline, dark center |
| Pop-up messages | Red outline, dark center. Failure messages too: firmware doesn't mark pop-ups by importance |
| Emergency screen | Stays a full red fill with black text |
| Boot splash | Not themed. It draws before the settings apply, to leave startup code unchanged, so a radio left in Night shows the usual bright boot screen first. The update-mode splash is red text around the fixed logo |

Still open:

- Backlight: the 10% minimum is fine on the radio, so no separate Night brightness or lower minimum for now.
- The RX / TX LEDs at the top: two plain on / off pins (hardware-dm32.md 5, 6), so dimming would need PWM. Open: do they
  bother in the dark? If so, a Night setting could turn them off.
- Checked on the real panel (2026-11): the disabled rows were too dark, now brighter (`line` 64 to 96); the rest reads well.
- Spanish and Italian strings need a native speaker's check.
- Idea: switch to Night automatically after sunset, from GPS position and time.

### Satellite screens

Built: Tools > Satellites (OK 6 0) and a satellite's passes ([ui.md](spec/ui.md) 2.6, [user guide](user/24-satellites.md)),
from the mockups agreed on 2026-11-03. Decided there: tracking tunes the radio temporarily and puts the channel back
on leaving; times are local through a Time zone setting (no daylight saving rule); a Lowest pass setting (10° at
first); Satellites is Tools 0; pass alarms come later; arrows show each pass's way across the sky. The Tracking screen is built too (OK on a pass, or a long OK on a satellite
in the list; it is the fifth level, so not field use). Still to build: tuning the radio to the corrected frequencies
(receive first), the SO-50 arming countdown and key, and a shorter way in such as a programmable key.

Checked on the DM-32UV (2026-11-05): the ISS's next pass on the list matched an independent tracker's rise time and
peak, and three satellites filled in within 6 to 8 seconds of opening the list. The orbit model costs 3.76 ms a run.

With the Tracking screen the static stack check still passes (least free: the idle task, 132 B; `ui` 264 B, `sat`
152 B, `host` 924 B), and on the radio `tasks.heap_min_free` stayed 15,416 and the least free stack was `sat` at 272 B.

### UI redesign

A cleaner UI than stock, entirely in software, built in the simulator. The current look is liked, so the scope is to be
decided.

- How: edit the UI code and preview with `make -C tests/host ui-gallery`.
- Constraint: 16 colors.
- Unknown: how far theming goes before it means editing layout code.

### On-radio orbit prediction (SGP4)

The radio computes satellite positions and passes from orbital elements (TLEs), with no PC during operation.

- Algorithm: SGP4, near-earth only (every FM satellite), written from the published papers
  ([satellite.md](spec/satellite.md) 3). Without an FPU, measure on the radio; one update a second is the requirement,
  and a day of passes for one satellite takes about 1,600 runs.
- Precision: double precision. Single precision drifts a few meters a day, and up to 5-6 km over a month.
- Inputs: TLEs (about 140 bytes per satellite), time from the RTC or GPS, position from GPS or a typed grid square.
- Outputs: pass list with rise, peak and set times, maximum elevation, live azimuth and elevation, range rate for
  Doppler.
- Test: done as a host test against python-sgp4 and Skyfield: positions within 1e-9 km, look angles within 0.004
  degree, rise and set within 0.6 s.
- Time: no driver reads the RTC yet, so UTC comes from GPS NMEA (RMC) for now, up to about a second late.

### Automatic Doppler correction

The main feature. The radio retunes receive and transmit continuously during a pass.

- How: each second, take the range rate from SGP4, compute the shift for downlink and uplink, write the new frequency.
- Resolution: 10 Hz units, so correction is effectively smooth.
- Transmit: the single synthesizer retunes when keyed, so the corrected uplink applies at each key-up.
- Feasibility: high in principle. Test receive-only first.
- Unknown: whether retuning during receive causes clicks or audio drops.

### Drift detection and frequency calibration

The receiver measures how far the downlink sits from the predicted frequency, shows it, and corrects live. This also
hides slightly stale orbit data.

| Cause of the offset | Affects | Handling |
|---|---|---|
| The radio's crystal error | Receive and transmit | Calibrate once against a known frequency, such as a local repeater, then apply to both. Frequency trim already exists on the radio |
| The satellite's transmitter error | Receive only | Correct receive only. SO-50's downlink is known to jump, a good test case |
| Orbit-data error | Receive and transmit | Correct both from the measured receive offset |

- How to measure: demodulated FM carries a DC term proportional to the carrier offset, and the FD6818 has an AFC loop.
  Upstream issue [#244](https://github.com/RadioFoundry/RadioFoundry/issues/244) explores this.
- Limit: the uplink can't be measured directly.
- Display: a live "drift: +1.2 kHz" on the pass screen.
- Unknown: whether the reading is fast and steady enough during speech. It will likely need averaging.

### Updating orbit data without a computer

TLEs go stale within weeks. The radio shows each TLE's age and warns when it is old (30 days now; about two weeks
suits FM satellites better, and should become a setting).

Built: `rfoundry_debug.py sat update` downloads CelesTrak's amateur-satellite file and brings the stored satellites up
to date over the cable, keeping their profiles ([user guide](user/24-satellites.md#keeping-the-orbit-data-current)).
Next, in order: a Satellites page on the web site with the same job (desktop Chromium, Web Serial), keypad entry, sound
transfer.

| Option | How it works | Works with | Status |
|---|---|---|---|
| Sound transfer | A web page plays a data tone into the mic, and the radio decodes it without transmitting | Any phone, no cable | Needs mic samples with the transmitter off. Untested. The stock VOX mic level reading is suspect ([hardware-dm32.md](spec/hardware-dm32.md)) and `VOX` isn't written here, so check this early |
| Phone cable | A web page sends TLEs over USB OTG and the programming cable | Android with Chrome 148 or later | See below |
| Keypad entry | Type the TLE digits. Each line's check digit catches typos | Always | About 130 digits per satellite. Tedious, but a guaranteed fallback |

### Phone companion over cable (Android)

A phone moves data in and out. It doesn't run Doppler, because the radio does.

- How: a web page using Web Serial, a USB-C OTG adapter and the programming cable. Chrome 148 beta added Web Serial on
  Android in April 2026.
- Jobs: push TLEs, set time and location, pull logs, recordings and images.
- iPhone: not possible this way, because Safari has no Web Serial. Use sound transfer.
- Phone as microphone: not recommended. Audio, PTT and data would share one wire.
- Unknown: whether Web Serial is in stable Chrome for Android, and whether it supports FTDI cables there.
- Wanted: refreshing TLEs from a phone in the field (an OTG adapter and the cable). The site already has a debug link
  client over Web Serial (`site/static/js/flasher/link.js`), so the page is a port of `sat update`. First test, with the
  radio: open the Codeplug Editor in Chrome on the phone, plug the cable in through the OTG adapter and press Connect.
  If the radio's port appears, the page will work; if not, the fallbacks are a small Android app on the USB-serial
  library, or a Termux script. Also open: whether the browser may fetch CelesTrak's file (CORS), else the site hosts a
  daily copy or the page takes a saved file.

### Satellite profiles and SO-50 auto-arm

Each satellite gets a stored profile, so picking one sets every frequency and tone.

- Contents: name, uplink, downlink, uplink tone, mode, special steps.
- SO-50 auto-arm: SO-50 needs a 2 s carrier with a 74.4 Hz tone to arm a 10 minute timer, then 67 Hz for contacts. One
  key sends the arm burst, switches to 67 Hz and shows a 10 minute countdown.
- Feasibility: high. Codeplug data plus a small state machine, which suits a host test.
- Profiles must be editable on the radio and from a phone, because satellite status changes.

| Satellite | Uplink (MHz) | Downlink (MHz) | Notes |
|---|---|---|---|
| ISS crossband repeater | 145.990, 67 Hz | 437.800 | |
| SO-50 | 145.850 | 436.795 | Arming above |
| AO-91 | 435.250 | 145.960 | Daylight only |

Check these against [AMSAT](https://amsat.org/?p=5012) before use.

### Pass alarms

The radio beeps a set time before a pass and loads that satellite's profile if chosen. The operator can dismiss it.

- Filter by elevation: alert only above a chosen maximum elevation, since a handheld needs high passes.
- Filter by satellite: favorites only.
- Depends on orbit prediction.

### Auto pass setup

When a pass starts the radio applies satellite settings, then restores the user's when it ends.

- Squelch: open wide, as FM satellite guides recommend for weak downlinks.
- Filter: wide (25 kHz), to tolerate residual Doppler.
- Optional: raise power at low elevation, lower it near the peak.

### Pass signal plot with contact markers

The radio draws received signal strength across the pass.

- Transmit periods: red gaps, since no reading is possible while transmitting.
- Contact marker: a button press drops a marker, which can start a logbook entry.
- Elevation overlay: the predicted curve behind the plot, to tell antenna problems from geometry.
- Storage: saved with the pass log.

### Voice-recorded callsigns and pass recording

Press a button and say a callsign into the mic, without transmitting. Stock firmware has a record function (back in
firmware 049), so the hardware can record.

- Callsign clips: a few seconds each, attached to the logbook entry.
- Whole-pass recording: with the DMR voice codec at roughly 2.4 kbps (an estimate), a 10 minute pass is about 180 KB.
  Received audio and your own transmissions could share one file.
- Unknown: whether mic audio can be captured with the transmitter off, and how much data flash is free.

### ID timer

A reminder or an automatic Morse ID, so you identify on schedule.

- Toggle: easy on and off, for example during a long contact.
- Modes: a reminder beep, or a short tone-keyed callsign sent automatically.
- Shares its Morse engine with CW and Morse below.

## Stretch goals

### Satellite logbook and ADIF export

Satellite, time, frequency and grid square fill in automatically. The callsign is typed, or a voice clip is attached.
Export ADIF for upload to LoTW or other logs. Text records in data flash.

### ISS APRS mode

During an ISS pass, APRS switches to the ISS digipeater settings, then back. `APRS` already works both ways on this
radio.

- Settings: 145.825 MHz, with RS0ISS, ARISS or APRSAT as the path.
- Behavior: beacon only while the ISS is in view, at a polite rate. List the stations heard through it. The operator
  can also send a packet by hand.
- Doppler: usually within FM tolerance for 1200 baud on 2 m, so correction is optional.

### SSTV receive

Plausible, and a real chunk of work: the decoder and the display path are both new.

- Audio: the CPU can get FM receive audio through the main chip.
- Modes: Robot 36 is 320x240, the screen in landscape. PD120, the usual ISS mode, is 640x496 and would be scaled.
- Display: needs full color written straight to the display, past the 16-color palette.
- Saving: store the full image in data flash and pull it off by phone.
- Doppler: matters on UHF events, such as the 2026 series on 437.825 MHz.

### Computer control

`NET_RIGCTL` already exists: Hamlib's protocol on port 4532, over PPP on the K1 cable
([tcp-services.md](spec/tcp-services.md)). Lower priority now that Doppler runs on the radio, but useful for logging and
other software.

- Limit: `\set_freq` sets the VFO simplex, and `rigctld` reports no split. A tracker can steer receive, but it can't
  manage separate uplink and downlink frequencies without new work.
- Limit: PTT shares a wire with serial data, so a speaker-mic and computer control can't be used together, and false PTT
  presses are possible.
- Allows without transmitting: logging software reading the frequency, a program such as Gpredict steering receive,
  monitoring from a computer.
- `\set_ptt` exists and keys only from the computer the radio is plugged into. Whether it works on this radio's shared
  line is untested.

### Spectrum view

A stepped sweep: tune across a range, read RSSI at each step, draw bars. It isn't a live waterfall, because the radio
can't receive normally while sweeping, and the readings are uncalibrated. Settling time after a retune sets the sweep
speed and how useful this is.

### CW and Morse

The radio can probably send Morse but can't receive real CW, because the FD6818 has no beat-note receive mode.

| Feature | What it does | Feasibility |
|---|---|---|
| Practice mode | Type text, or use the side keys as a paddle. Morse plays only through the speaker | High, no transmit risk |
| Tone-keyed Morse (MCW) | Morse as an audio tone on FM, heard by any FM radio, including through the ISS repeater | High. Mixing with a 67 Hz access tone is unverified |
| Carrier-keyed CW | The transmitter keyed on and off | Plausible. Needs clean power ramping, checked on a spectrum analyzer |
| Inverse-CW decoder | Watches noise and signal readings, plays its own tone when a carrier appears, could decode to text | Experimental, strong signals only |

The side keys could act as an iambic paddle, with stored messages (CQ, callsign, grid square) on keys. Check FCC Part 97
for MCW on the frequencies you plan to use.

## Idea backlog

Satellite:

- Telemetry capture: reuse the APRS decoder to log 1200 baud packets from cubesats and export them.
- Sun and moon: sunrise, sunset and moon phase from the same GPS and time data.

Other:

- Grid square from GPS on the main screen.
- Nearest-repeater finder from a stored list by GPS position, setting offset and tone.
- Activity list: recently heard frequencies with signal strength.
- Coverage logging: signal strength with GPS position, to map a repeater's coverage.
- Mode profiles: one-key switching between presets such as travel, satellite and hotspot.
- Macros: user-defined key actions without rebuilding.
- Voice announcements of frequency and menus, for accessibility.

## Open questions

| Question | Status |
|---|---|
| Does the CK803S have an FPU? | Built without one (`-msoft-float`). SGP4 costs 3.76 ms a run on the radio (measured 2026-11-05): far inside one update a second |
| Flash and RAM left in the image | The standard image has 16 B of main RAM free; the configuration above has 23,928 B free (measured 2026-11-05) |
| Can the RTC be read and set? | Open. Registers are in [hardware-dm32.md](spec/hardware-dm32.md) 3.3; no driver yet |
| Does `rigctld` set frequency on this radio? | Written, and sets the VFO simplex. No result recorded on the radio yet |
| Does the GPS get an outdoor fix? | NMEA arrives on the radio. A fix outdoors is still to check |
| Board revision, stock firmware version, vocoder variant | Open: needs the radio |
| Data flash free for profiles, TLEs, logs and recordings | Open |
| Does retuning during receive cause clicks or audio drops? | Open |
| How fast does the FD6818 settle after a retune? | Open |
| Can the CPU read mic samples with the transmitter off? | Open. The VOX mic level reading is suspect |
| Can an offset reading be fast and steady enough for drift correction? | Open |
| Can the FD6818 send a Morse tone and the 67 Hz tone together? | Open |
| How cleanly does the power amplifier switch for carrier-keyed CW? | Open: needs a spectrum analyzer |
| Is the radio's own PTT button on the serial line, or only the jack's PTT pin? | Open |
| Can the firmware key the transmitter from a serial command with the cable connected? | Open |
| Is Web Serial in stable Chrome for Android, and does it work with an FTDI cable? | Open |

## Sources

- [RadioFoundry DM-32UV hardware reference](spec/hardware-dm32.md)
- [RadioFoundry PR #270: FD6818 drivers](https://github.com/RadioFoundry/RadioFoundry/pull/270)
- [RadioFoundry issue #244: frequency trim](https://github.com/RadioFoundry/RadioFoundry/issues/244)
- [M7OCM DM-32UV archive](https://github.com/M7OCM/DM-32UV): stock firmware and CPS
- [AMSAT FM satellites](https://amsat.org/?p=5012)
- [AMSAT-UK: how to work FM satellites](https://amsat-uk.org/beginners/how-to-work-fm-satellites/)
- [ARRL: ISS APRS paths](https://www.arrl.org/news/simultaneous-aprs-and-voice-repeater-on-the-international-space-station)
- [ARISS SSTV events](https://www.ariss.org/upcoming-sstv-events.html)
- [Web Serial browser support](https://www.testmuai.com/web-technologies/web-serial/)
