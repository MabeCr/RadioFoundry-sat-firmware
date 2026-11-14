<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 The RadioFoundry Authors -->

# DM-1701 satellite build: bring-up

For the DM-1701's satellite configuration (`CONFIG_SAT` and `CONFIG_SAT_SOUND`, [satellite.md](../spec/satellite.md),
[sound-transfer.md](../spec/sound-transfer.md)): the build, the install and what was seen on the radio. Nothing here
transmits. Section 4 is a dated log; nothing in it is a support claim.

## 1. Build and measure (no radio)

The satellite build is the radio's own defaults plus tracking and sound receive (`SATELLITE_VARS` in
`boards/dm1701/config.mk`); nothing is cut. CI builds and stack-checks it on every push and publishes it with the other
satellite images (`.github/workflows/satellite-image.yml`).

| Step | Command | Expect |
|---|---|---|
| Compiler | `arm-none-eabi-gcc --version` | the pinned release (`platform/stm32f4/platform.toml`, 13.3.rel1). A newer GCC uses more stack: the default build's `host` and `radio` tasks fail the stack check with GCC 15 |
| Build | `tools/satellite.sh dm1701` (`CROSS=` when the compiler isn't in `~/opt`) | no warnings; `build/dm1701-satellite/rfoundry_dm1701.bin` |
| Stack check | `tools/satellite.sh dm1701 stack-check` | `every task fits` |
| Host tests | `make -C tests/host run-sat` | every satellite test prints `passed` |

## 2. Install

The image has no codec: a radio already on RadioFoundry keeps its own. In the [web installer](../user/02-installing.md),
**Install**, the DM-1701, then on the Build step **Use my own firmware build** and the `.bin`. Take the **full backup**
when asked: on a radio that ran an OpenGD77-family firmware, the satellite area at 0x01B000 holds that firmware's
bytes ([hardware-dm1701.md](../spec/hardware-dm1701.md) 22), and the installer clears it only after a full backup.

## 3. Steps on the radio

1. `rfoundry_debug.py sat show`: `sat.built=1`, `sat.store=ours`, `sat.slots=32`. `foreign`: another firmware's bytes
   in the area; `rfoundry_debug.py storage dump --out DIR` saves them, `sat clear` clears them.
2. Settings > Location > Set time, in UTC. The DM-1701 has no battery-backed clock: set it after every power-on
   without a GPS fix.
3. On a DMR channel, Tools > Satellites > Receive sound, and play the TLE page's sound at the microphone.
4. Power off and on: the stored satellites are still listed.
5. A satellite's passes and its Tracking screen.

## 4. What was seen on the radio

| Date | What | Result |
|---|---|---|
| 2026-11-14 | The satellite build, GCC 13.3.rel1 | 550,844 B flash (74.7 %), 121,736 B RAM (9.3 KB free); stack check: every task fits, `ui` 0 B over the 128 B margin |
| 2026-11-14 | Installed with the web installer's own-build upload over RadioFoundry | installed; the radio's settings and fixed APRS position kept |
| 2026-11-14 | Receive sound, first time | "No place to keep satellites": `sat.store=foreign`. The radio ran an OpenGD77-family firmware before RadioFoundry; its backup from 2026-11-06 had every sector of 0x010000-0x01EFFF written. `sat clear` made it `ours` (the bytes are in that backup). The claim and the installer now clear the area, and the screen names the cause |
| 2026-11-14 | Receive sound, the ISS alone | received and stored |
| 2026-11-14 | Set time (Settings > Location), then Tracking | tracks with the fixed APRS position |
| 2026-11-14 | Power off and on | the stored satellite kept; the time lost (no battery-backed clock) |
| | Receive sound with 32 satellites, `sat bench`, a Tracking soak, a real pass with the automatic mode | **untested** |
