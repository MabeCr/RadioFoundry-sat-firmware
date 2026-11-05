<!-- SPDX-License-Identifier: MIT -->

# DM-32UV satellite build: size check and first install

A checklist for the first day with the radio. Part A needs only your computer, Part B needs the radio. Do them in
order, and stop at any step that says STOP. Nothing here transmits. The plan, the decisions and the estimated RAM
budget are in [dm32-satellite-plan.md](dm32-satellite-plan.md#does-it-fit-estimate-2026-11-03).

The question to answer: does the satellite configuration build, fit in the DM-32UV's RAM, and keep every task's stack
and the RTOS heap inside their limits? The estimate says about 20 KB of main RAM is left. Part A turns the estimate
into a measurement before the radio is touched.

## Part A: build and measure (no radio)

### A1. Get the branch

```bash
git clone https://github.com/MabeCr/RadioFoundry-sat-firmware
cd RadioFoundry-sat-firmware
git checkout claude/peaceful-ride-m0ie2u
git submodule update --init
```

### A2. Get the C-SKY compiler

Pick one. The first is quicker if you have Docker.

| Route | How | Time |
|---|---|---|
| Docker | `tools/docker/csky-toolchain/run.sh make BOARD=dm32` runs the project's compiler image (it pulls it once). Put `tools/docker/csky-toolchain/run.sh` in front of every `make` below | minutes |
| Build it | Install the build tools (Ubuntu: `sudo apt install build-essential libgmp-dev libmpfr-dev libmpc-dev libisl-dev texinfo bison flex`), then `tools/build-csky-toolchain.sh`. It installs into `~/toolchains/csky-elf`, which the Makefile finds by itself. macOS needs the notes at the top of the script | about 45 minutes |

Check it: `~/toolchains/csky-elf/bin/csky-elf-gcc --version` (or the Docker route's `... run.sh csky-elf-gcc --version`)
says GCC 14.

### A3. Build the baseline: the standard DM-32UV image

This is the radio as upstream builds it. It checks that your compiler gives upstream's numbers, which makes every
later number trustworthy.

```bash
make BOARD=dm32 BUILD=build/dm32-base 2>&1 | tee build-base.log
grep -E "FLASH:|RAM:" build-base.log
```

Write down the linker lines (`FLASH:` and `RAM:`; the DM-32UV has one RAM region) (used / region size / percent). Measured 2026-11-05: the image about 587 KB,
main RAM only 16 bytes free. The satellite build: 454 KB and 23,928 bytes free (done: see the plan). If your build leaves several KB more or less free, say so before going on:
the compiler or the checkout differs.

### A4. Build the satellite configuration

```bash
make BOARD=dm32 BUILD=build/dm32-sat \
	CONFIG_SFR=n CONFIG_DMR_PRIVACY=n CONFIG_DMR_TMS=n CONFIG_DMR_ARS=n CONFIG_DMR_LRRP=n \
	CONFIG_DMR_CONTROL_STATION=n CONFIG_SNMP=n CONFIG_HOTSPOT_SPLIT=n CONFIG_LANG_ES=n CONFIG_SAT=y \
	CONFIG_NET_USRP=n CONFIG_HOTSPOT=n CONFIG_ZONE_SHARE=n CONFIG_NET_HTTP=n CONFIG_DMR_ROAMING=n \
	2>&1 | tee build-sat.log
grep -E "FLASH:|RAM:|warning|overflow" build-sat.log
```

This is the first compile of the satellite code with the C-SKY compiler. Two things can happen:

- It builds with no warnings: go on.
- Errors or warnings: STOP and send me `build-sat.log` (`grep -B2 -A6 -E "error|warning" build-sat.log`). Likely causes
  are a difference between the two compilers, such as `-Wdouble-promotion` or a missing math function in newlib.

### A5. Read the numbers

| From the linker lines | What to compare | Go if |
|---|---|---|
| `RAM:` (main, 211,456 B) | free bytes in the satellite build | at least about 4 KB free (the estimate says 20 KB) |
| `FLASH:` | image size | under 880 KB (it will be far under) |

The difference between `build-base.log` and `build-sat.log` is the real cost of the whole configuration, switches
off and satellites on. Compare:

```bash
make BOARD=dm32 BUILD=build/dm32-sat size   # section sizes and the space left before the codec
```

If main RAM is more than 4 KB short of the estimate, the cheapest ways out are in the plan's "Further cuts" table
(`TCP_CONNS=2` is about 1.5 KB, `DUAL_WATCH` 5.3 KB, `APRS_RX` 7 KB).

### A6. The stack check

The static check bounds every task's stack from the compiled code. It is what the ARM build's one tight spot (the UI
task) needs checking against the C-SKY compiler's frames.

Run the A4 command again with `stack-check` added as its last word (it reuses the build). Expect `every task fits`. Note the least free (`ui`
was 0 bytes on the ARM build; the check itself keeps a 128 byte margin).

- A task shown `OVER`: STOP. Most likely the UI (`ui`) or the host task (`hostlink_task`). The fix is a larger stack for
  that task (`UI_TASK_STACK_WORDS` in `core/ui/ui.c`), paid for out of the RAM A5 found left over. Send me the
  `stack-check` output.

### A7. Run the host tests once more on your computer (optional, a minute)

```bash
make -C tests/host run-sat
```

All four satellite tests print `passed`.

If A3 to A6 are fine, the configuration fits on paper and the check is done. Everything after this checks it on the
radio.

## Part B: the radio

### B1. Back up first

Before any install, follow [dm32-bringup.md](testing/dm32-bringup.md) section 0 step 1: **back up both flashes** (the
1 MB program flash and the 16 MB data flash), and keep two copies. The data flash has this radio's factory calibration;
no other radio's backup replaces it. Backups never go in a git tree.

Also: use an **FTDI cable** if you have one (a CH340 can fail partway), and have the stock firmware file for the
codec and the way back.

### B2. Install the stock RadioFoundry release (your first plan)

Use the web installer at radiofoundry.org: Install, pick the DM-32UV, and follow it (it takes the backup for you if the
radio still runs the stock firmware). See [Installing](user/02-installing.md#install-with-the-web-installer) and the
DM-32UV's section of the [Radio reference](user/23-radios.md). Update mode is **PTT + SK1** held while turning the
radio on.

Check it afterwards, with the antenna on and no PTT:

- It boots to Home, the keys and knob work, FM receives.
- Over the programming cable the debug link answers:

  ```bash
  tools/rfoundry_debug.py version                  # board=DM-32UV, features with HOSTLINK and DEBUGLINK
  tools/rfoundry_debug.py view --section tasks     # note tasks.heap_min_free and tasks.min_free
  ```

If this fails, nothing about the satellite build matters yet: use [dm32-bringup.md](testing/dm32-bringup.md) and the
installer's help, and go back to the stock firmware (B7) if you need to.

### B3. The baseline on the radio

Still on the stock release, use the radio for 10 minutes (change channels, open Tools, Settings, Diagnostics), then:

```bash
tools/rfoundry_debug.py view --section tasks
```

Write down `tasks.heap_min_free` (upstream saw 8,664 bytes on a DM-32UV after a long test run; it must stay above
2,048) and `tasks.min_free`. This is the number the satellite build is compared with.

### B4. Flash the satellite build over it

The standard release is a RadioFoundry radio, so rfoundry keeps its codec. Dry run first:

```bash
rfoundry flash build/dm32-sat/rfoundry_dm32.bin --board dm32 --dry-run
rfoundry flash build/dm32-sat/rfoundry_dm32.bin --board dm32
```

(Get `rfoundry` from the release page, or `pipx install tools/rfoundry`. See [Installing](user/02-installing.md#advanced-rfoundry-on-the-command-line).) The
dry run shows which flash sectors it will write; it must leave the bootloader (the first 48 KB) and the codec slot
alone. Put the radio in its bootloader when it asks. If the radio stays dark afterwards: update mode again and
reinstall the release (B7).

### B5. Check the satellite build

```bash
tools/rfoundry_debug.py version                    # the features list now has SAT
tools/rfoundry_debug.py view --section tasks       # a "sat" task; heap_min_free still above 2,048
tools/rfoundry_debug.py view --section sat         # sat.store=ours, sat.slots=32
python3 -m pytest tests/hil/test_task_stacks.py tests/hil/test_sat.py -v -rs --radio-port /dev/ttyUSB0
```

(Use your cable's port name; on macOS something like `/dev/cu.usbserial-XXXX`.) What to look for:

| Check | Pass |
|---|---|
| `tasks.heap_min_free` after the same 10 minutes of use as B3 | above 2,048, and not much below B3's (the build's heap is 2 KB larger, and the satellite task takes 1.6 KB of it) |
| `test_task_stacks.py` | passes |
| `test_sat.py` | passes. It prints the **time per model run** on this CPU: the open question of SGP4 without a floating-point unit. Under 100 ms a run is fine; 10 to 30 ms is expected |
| Tools shows **0 Satellites** | the row is there; open it: "No satellites stored" |

The speed is the other number the plan needs. Write down the `sat_us_per_run` line from the test, or run
`tools/rfoundry_debug.py sat bench` yourself.

### B6. A satellite on the screen

Indoors, no GPS fix is needed for this:

```bash
tools/rfoundry_debug.py sat load iss.tle           # a file with the ISS's two TLE lines (CelesTrak or AMSAT)
tools/rfoundry_debug.py sat save                   # stored with the built-in ISS profile
tools/rfoundry_debug.py sat time                   # the radio's clock from this computer's
tools/rfoundry_debug.py sat observer 40.0 -105.3 1650   # your latitude, longitude, height in meters
```

Then Tools > Satellites on the radio: the ISS has a next pass, with its time and an arrow. OK opens its passes. After
a minute the screens fill in; a bad TLE (a typo) is refused by the load with a message.

Outdoors with a GPS fix, the time and position come by themselves (the screen first says "Waiting for the time").

If the screens come up wrong, send me a screenshot: `tools/rfoundry_debug.py screenshot -o sat.png`.

### B7. The way back

- To the standard release: put the radio in its bootloader and run the web installer's Install again (or `rfoundry
  flash --release latest`).
- To the stock Baofeng firmware: [Installing, going back to the stock firmware](user/02-installing.md#going-back-to-the-stock-firmware).
- If nothing boots: update mode (PTT + SK1 at power-on) always works, because the bootloader is never written.

## What to send me afterwards

- From A3 and A4: the `FLASH:` and `RAM:` lines of both builds, and the stack-check output.
- From B3 and B5: `tasks.heap_min_free`, and the model's time per run.
- Anything that didn't pass, with the log.

With those, I update the plan's budget table from estimates to measurements, and we go on to tracking.
