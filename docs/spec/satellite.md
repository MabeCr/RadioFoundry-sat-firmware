<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 The RadioFoundry Authors -->

# Satellite tracking

`CONFIG_SAT` (default off). The radio predicts where an amateur satellite is from its two-line elements (TLE): when it
rises over the radio's position, where to point the antenna, and how far Doppler moves its uplink and downlink. It
also keeps a profile per satellite and arms a repeater that needs a wake-up carrier. Which radios build it:
[feature-matrix.md](../feature-matrix.md).

Status: the library in `core/sat` and its host tests; stored satellites (6.3); the debug link's SAT command, which loads
and stores element sets and runs the model on the radio ([debuglink.md](debuglink.md) 3.24); and Tools > Satellites,
the satellite list and pass list, worked out by a task of their own (5.1, [ui.md](ui.md) 2.6). The Tracking screen:
the opened satellite's position every second, the pass's path for a sky plot, and the Doppler-corrected frequencies
(5.1). Applying Doppler to the radio is not built yet. Section 9 lists what is still to write.

| Section | Topic |
|---|---|
| 1 | Files and layers |
| 2 | Two-line elements |
| 3 | The orbit model (SGP4) |
| 4 | Look angles and Doppler |
| 5 | Pass prediction |
| 6 | Profiles and repeater arming |
| 7 | Cost |
| 8 | Testing |
| 9 | Not written yet |

## 1. Files and layers

Everything in `core/sat` is portable C with no RTOS calls and no board header. The caller owns every struct; the
static state is the UTC clock (4.1) and the service's results (5.1). The host tests compile it unchanged.

| File | Does |
|---|---|
| `sat_tle.c` | TLE lines to `sat_elements_t`, check digits, epochs, civil dates to day counts |
| `sgp4.c` | The orbit model: `sgp4_init` once per element set, `sgp4_propagate` per time |
| `sat_math.c` | Sine and cosine for the model (3.2) |
| `sat_look.c` | Sidereal time, the observer, azimuth / elevation / range / range rate, Doppler, Maidenhead locators |
| `sat_pass.c` | The next pass: rise, highest point, set |
| `sat_profile.c` | The built-in profiles and the arming state machine |
| `sat_clock.c` | UTC from NMEA date and time, for the model (4.1) |
| `sat_svc.c` | The service: every stored satellite's next pass and one satellite's pass list, a slice at a time, and the tracked pass's position and path (5.1) |
| `core/app/sat_task.c` | The service's task: its inputs (clock, position, setting), a poll, a sleep |
| `core/ui/ui_svc_sat.c`, `core/ui/ui_sat.c` | What the screens read, and the screens ([ui.md](ui.md) 2.6) |
| `sat_track.c` | One tracked satellite: its model, the observer, its profile, and the look and Doppler at a time |
| `sat_store.c` | Stored satellites in the board's data flash (6.3); the one file here that uses `core/storage` |
| `core/debuglink/debuglink_sat.c` | The debug link's SAT command and `sat` view section ([debuglink.md](debuglink.md) 3.24) |

## 2. Two-line elements

The 69-column NORAD format (two lines, columns fixed). `tle_parse` reads only the fields SGP4 uses into
`sat_elements_t` (64 bytes), so a stored satellite costs 64 bytes, not the 140 of text.

| Rule | Detail |
|---|---|
| Line shape | Column 1 is the line number (`1`, `2`), column 2 a space, at least 69 columns. A line that ends sooner (a NUL, CR or LF) is `TLE_ERR_LENGTH`; nothing after column 69 is read |
| Check digit | Column 69 of each line: the sum of the digits in columns 1-68, plus 1 for each `-`, modulo 10. A mismatch is `TLE_ERR_CHECKSUM`. This is what catches a typo in keypad entry |
| Catalog number | Columns 3-7 on both lines, which must agree (`TLE_ERR_MISMATCH`). Alpha-5: a leading letter stands for 10-33 (A-Z without I and O), so `A0001` is 100001 |
| Epoch | Columns 19-32: two-digit year (57-99 = 1957-1999, 00-56 = 2000-2056) and the day of year with its fraction, 1.0 = 1 January 00:00 UTC. Kept as Unix seconds (double) |
| Drag | B* in columns 54-61, in the format's exponent form: ` 12345-3` is 0.12345e-3 |
| Angles | Inclination, node, argument of perigee and mean anomaly in degrees; eccentricity with its implied leading point (`0006703` = 0.0006703); mean motion in revolutions a day |
| Numbers | A fixed-point reader, not `strtod`: the C library's float parser costs flash and accepts forms the format doesn't have. A letter in a number field is `TLE_ERR_FIELD` |

The first and second derivatives of the mean motion (line 1, columns 34-52) are not read: SGP4 doesn't use them.

## 3. The orbit model (SGP4)

### 3.1 Model

SGP4 as published in Hoots and Roehrich, *Spacetrack Report No. 3* (1980), with the corrections in Vallado,
Crawford, Hujsak and Kelso, *Revisiting Spacetrack Report #3* (AIAA 2006-6753). Written from the papers' equations;
the variable names are theirs so the code reads against them. No other implementation's source was used
([CLEANROOM.md](../CLEANROOM.md)).

| Choice | Value | Why |
|---|---|---|
| Constants | WGS-72 (mu 398600.8 km^3/s^2, radius 6378.135 km, J2, J3, J4) | The elements are fitted with WGS-72; other constants give a worse position |
| Precision | Double throughout | Single precision drifts kilometers over the life of an element set |
| Orbits | Near-earth only: period under 225 minutes | The deep-space terms (SDP4: lunar and solar perturbations, resonances) are as large again and serve no amateur FM satellite. A longer period is `SGP4_ERR_DEEP_SPACE` |
| Low perigee | Under 220 km: the simplified drag terms (`isimp`) | As the model defines |
| Errors | Eccentricity out of range, mean motion not positive, semi-latus rectum negative, radius under one earth radius (decayed) | Returned as `sgp4_err_t`; the caller shows the satellite as not trackable |
| Output | Position (km) and velocity (km/s) in TEME, the frame the model works in | Section 4 turns it to the observer |

`sgp4_t` holds the 46 doubles the model works out once per element set (376 bytes). `sgp4_propagate` takes minutes
since the epoch, negative allowed, and is a pure function of them.

### 3.2 Math functions

The C library's `pow`, `asin`, `fmod`, `sin` and `cos` would add about 10 KB of flash on a radio without a floating-point
unit. The model doesn't need their general cases:

| Instead of | `core/sat` uses | Exact because |
|---|---|---|
| `pow(x, 2/3)` | Newton's method for the cube root, squared | A near-earth orbit's argument is 0.5-3; it converges from 1 in under 8 steps to the last bit |
| `pow(x, 1.5)`, `pow(x, 3.5)`, `pow(x, 4)` | `x * sqrt(x)`, products | Identities |
| `fmod(x, 2 pi)` | `x - 2 pi trunc(x / 2 pi)` | The same, sign kept, for angles under 2^31 turns |
| `asin(u / r)` | `atan2(u, sqrt(s^2 + e^2))` | The same angle, better conditioned near the zenith |
| `sin`, `cos` | `sat_sincos`: reduction by pi/2 in three parts of at most 30 bits (exact for under 2^23 quadrants), then Taylor series to the 15th / 16th power on +-pi/4 | Agrees with the C library within 2.2e-16 over +-5000 rad (the host test). The library's general reduction for huge arguments is 3.4 KB on its own |

`atan2` and `sqrt` stay the C library's.

## 4. Look angles and Doppler

### 4.1 Time

The model needs UTC to about a second: a second's error moves a low pass by up to half a degree near its peak and its
70 cm Doppler by up to 100 Hz. `sat_utc` (`sat_clock.c`) takes the date and time of each valid NMEA RMC sentence, from
the board's GPS or from a phone or PC feeding NMEA over the host link ([aprs.md](aprs.md) 6), and stamps it with the
millisecond clock. A board with a battery-backed clock (`hal_rtc.h`) also restores it at start (4.1.1).

| Rule | Detail |
|---|---|
| Source | RMC with status A only, in `aprs_nmea` (so `CONFIG_APRS` carries it). GGA has no date |
| Year | Two digits, 2000 + yy |
| Accuracy | RMC names whole seconds and arrives a fraction of a second after the second it names: up to about a second late |
| Checks | Day, month, leap day, hour, minute and second (60 is a leap second, taken as the next second's start); a bad field changes nothing |
| Readers | Any task: a sequence count around the writer's two stores, so a reader never sees half an update. No lock, no RTOS call |
| Without a sentence | `sat_clock_now` is false: no prediction until the first fix |

The millisecond clock wraps after 49 days, harmless while sentences keep coming.

A reader that finds the sequence count odd or changed 50 times in a row gives up (`sat_clock_now` is false for that
call): the writer runs below some readers, and a reader that spins on a write it preempted would never let it finish.

### 4.1.1 The battery-backed clock

`hal_rtc_read` / `hal_rtc_write` (`hal/hal_rtc.h`) give a board's clock as a 16-bit day number and the time of day. They
are called from the satellite task only (`core/app/sat_task.c`), so the board code has no locking; a board without a clock
leaves them out and a weak version says "none".

| Step | Rule |
|---|---|
| Day number | Unix days plus `SAT_RTC_DAY_BASE` (0x8000), so it runs to the end of 2059. A clock that another firmware set counts from its own epoch, lands outside 2026..2059 and is taken as never set by this firmware, rather than giving a wrong date |
| At start | If the radio has no time yet and the RTC holds one of ours, `sat_utc` is set from it. A GPS sentence overrides it within a second of locking |
| Set by hand | Settings > Location > Set time ([ui.md](ui.md) 2.6) sets `sat_utc` and asks the task to write the RTC, within a second |
| Kept right | Every 10 minutes the task compares the two; a difference over 2 s (a GPS fix corrected the radio) is written over the RTC. A clock the RTC doesn't have is written |
| Failure | A clock that doesn't answer costs a few milliseconds (every bus wait is bounded) and the radio runs on without it |

The time is UTC. Writing another firmware's clock back needs that firmware's own epoch, so after installing ours a
radio's date in the stock firmware is wrong until it is set there again.

### 4.2 Look angles

`sat_look` turns a TEME state into what the operator sees from `sat_observer_t`:

1. TEME to earth-fixed: a rotation about the pole by Greenwich mean sidereal time (IAU 1982, `sat_gmst`), with the
   time taken as UT1. The velocity loses the earth's rotation (7.292115e-5 rad/s).
2. The observer on the WGS-84 ellipsoid from geodetic latitude, longitude and height (`sat_observer_set`).
3. The range vector in south / east / up at the observer: azimuth from north, clockwise, 0-360 degrees; elevation
   above the geometric horizon (no refraction); range in km; range rate in km/s, positive while the satellite moves
   away.

Polar motion and UT1 - UTC are left out. Against a library that applies them (8) the difference is under 0.004 degree
in azimuth and elevation and under 0.3 m/s in range rate.

### 4.3 Doppler

f in Hz and v the range rate:

| Direction | Frequency | At 7 km/s coming in |
|---|---|---|
| Receive a downlink sent on f | f (1 - v / c) | 437.800 MHz heard 10.2 kHz high |
| Send so the satellite hears f | f / (1 - v / c) | 145.990 MHz sent 3.4 kHz low |

Both round to the hertz; the radio's tuning step applies after. `sat_locator_to_deg` gives the center of a 4- or
6-character Maidenhead locator, for an observer typed in without a position fix.

### 4.4 The retune test

A Doppler correction retunes the receiver every few seconds, and a retune can drop the audio for as long as the synthesizer
takes to lock and the chip to settle: a property of the radio, measured before the correction is designed
([the plan](../dev/satellite-doppler-plan.md)). `radio/retune_test.c` steps the receiver between a base frequency and an
offset of it, `count` times, `gap` apart (the radio task runs it, FM only, on an idle receiver; receive only, nothing is
transmitted), and times each retune on the radio's own clock:

| Reading | Meaning |
|---|---|
| call | the `rx()` call itself |
| settle | from the call until the signal level is within 3 dB of its steady value on that frequency, for three reads in a row |
| lowest, noise, no carrier | the lowest level and highest noise reading meanwhile, and the reads in which the chip said no carrier |

The steady value on each frequency is measured first (300 ms after tuning it, eight reads averaged). A frequency without
a steady carrier gives the call times only. A `RETUNE_TEST` request on the debug link ([debuglink.md](debuglink.md) 3.24)
starts it and `RETUNE_RESULT` reads it; the radio goes back to the channel it was on. The test refuses while
transmitting, scanning, on a DMR channel, or in AM or broadcast receive. Take a steady FM carrier such as a weather
channel (162.400 MHz here) for a base frequency.

## 5. Pass prediction

`sat_pass_next(s, o, from, span, min_el, ...)` finds the first pass that hasn't set by `from` and rises within `span`
seconds, with a highest point of at least `min_el` degrees. A pass in progress counts: its rise is then before
`from`.

| Step | Rule |
|---|---|
| Below the horizon | Steps of 2 s per degree below, 20-300 s. A low earth orbit's elevation changes by less than 0.5 degree a second below the horizon (about 0.3 at the horizon for a 200 km orbit), so a step can't jump over a rise |
| Rise and set | Bisection between the samples either side of the horizon, to 1 s |
| Through the pass | 20 s steps, keeping the highest sample |
| Highest point | Golden-section search in the 40 s around the highest sample, to 1 s |
| Too low | A pass under `min_el` is skipped and the search goes on from 20 s after its set |
| In progress | From `from`, back in 20 s steps (at most 30 minutes) to below the horizon, then as above |

A pass shorter than 20 s peaks a fraction of a degree up and can be missed; nobody works it. A day of passes for one
satellite takes about 1,600 runs of the model (the ISS over a mid-latitude observer, 7 passes), most of them below the
horizon.

`sat_pass_t` keeps rise, highest point and set as Unix seconds, the highest elevation and the azimuths at rise and set
(32 bytes).

### 5.1 The service

`sat_svc.c` (`core/app/sat_task.c` polls it) works one piece at a time, in this order: the tracked pass's path, the
opened satellite's pass list, then each list row's next pass. Each poll does about 30 runs of the model; a piece keeps
its place between polls. The UI reads the results without waiting (a sequence count around each update).

| Result | What |
|---|---|
| Rows | each stored satellite: state, next pass, peak, azimuths, way across the sky; an up satellite's elevation every 2 s |
| Passes | the opened satellite's passes in the next 24 hours (8 at most) |
| Live (`sat_svc_live`) | the tracked satellite's azimuth, elevation, range and range rate once a second, with the receive and transmit frequencies of its profile corrected for Doppler (4.3) |
| Path (`sat_svc_path_pt`) | 24 points of azimuth and elevation, evenly from the pass's rise to its set, for the sky plot |

A reader never waits for a write: the service's task runs below the UI's, so a UI task spinning on a write it had
preempted would never let it finish (the watchdog caught this once on the radio: UI task, 3 s). It tries 50 times, then
gives up for that draw and asks again at the next. The slow arithmetic of a result (Doppler, heading) is done before its
write, which then holds only stores.

The screen asks for a pass with `sat_svc_track(aos, los)` at every draw. Nothing tells the service the screen has gone, so
it stops, and drops the results, five seconds after the last request. A new observer or pass starts the path over.

## 6. Profiles and repeater arming

### 6.1 Profiles

`sat_profile_t` is what choosing a satellite sets: a name (11 characters), the catalog number of the element set it
tracks with, uplink and downlink as the satellite uses them (Doppler applies on top), the uplink's CTCSS tone, and an
optional arming step (6.2).

The built-in list is a starting set, taken from AMSAT's list of FM satellites (November 2026). Satellites fail and
change modes, so check it against AMSAT before use; the user's own list is to replace it (9).

| Name | Catalog | Uplink (MHz) | Downlink (MHz) | Uplink tone | Arming |
|---|---|---|---|---|---|
| ISS | 25544 | 145.990 | 437.800 | 67.0 Hz | — |
| SO-50 | 27607 | 145.850 | 436.795 | 67.0 Hz | 74.4 Hz for 2 s, awake 10 minutes |
| AO-91 | 43017 | 435.250 | 145.960 | 67.0 Hz | — (in sunlight only) |

### 6.2 Arming

Some repeaters sleep until a carrier with a different tone wakes them for a set time. `sat_arm_t` runs that on the
caller's millisecond clock and says what the radio should do; the radio core keys the transmitter.

| State | Output | Moves on when |
|---|---|---|
| `SAT_ARM_IDLE` | no key; tone = the uplink tone | `sat_arm_start`: to SENDING. Refused for a profile without arming |
| `SAT_ARM_SENDING` | key; tone = the arming tone | the arming time has passed: to ARMED. `sat_arm_abort` (PTT released early, the transmitter refused): back to IDLE, not armed |
| `SAT_ARM_ARMED` | no key; tone = the uplink tone; seconds left, rounded up | the awake time has passed, counted from the carrier's end: to IDLE with `expired` set |

`sat_arm_start` while armed sends a new carrier and restarts the awake time. Times compare by difference, so the
millisecond clock's wrap is harmless.

### 6.3 Stored satellites

`sat_store.c` keeps satellites in the board's data flash: each one's profile and element set in a record of 128 bytes
with its own CRC, 32 to a 4 KB sector, in the area the board gives (`hal_flash_map_t.sat_addr` / `sat_size`,
[settings-store.md](settings-store.md) 1). A board without the area keeps satellites in RAM only (the tracker). Which
boards have one: [feature-matrix.md](../feature-matrix.md).

| Rule | Detail |
|---|---|
| Slots | A slot is free (all 0xFF), a record (magic, layout and CRC right) or anything else |
| Ours or foreign | The area is ours while every slot is free or a record. One slot of anything else (another firmware's bytes) makes it foreign: records still read, nothing is written until it is cleared (CLEAR on the debug link, behind a token, after a backup) |
| Write | A change reads the record's sector into the shared sector buffer, changes the record and rewrites the sector through the journal ([settings-store.md](settings-store.md) 4): a power cut leaves the old sector or the new one. Refused while the buffer is taken (a codeplug write) |
| Put | Replaces the record with the same catalog number in place, else takes the first free slot; refused when full |
| Delete | The record's slot back to all 0xFF |
| Elements | Kept as the TLE gives them (degrees, revolutions a day), so a stored set drives the model to the same state bit for bit |
| RAM | None: records are read from flash when asked for |

Flash writes block for an erase and a program (tens of milliseconds). The debug link runs them in its task; a screen
must hand them to the housekeeping task (`storage_job_post`).

### 6.4 The record

Little endian; doubles are IEEE 754 binary64. The CRC is the storage layer's CRC-32 over bytes 0-123.

| Offset | Size | Field |
|---:|---:|---|
| 0 | 4 | magic `0x5441534F` (bytes `4F 53 41 54`, "OSAT") |
| 4 | 1 | layout, 1 |
| 5 | 1 | flags: bit 0, an element set follows |
| 6 | 2 | 0 |
| 8 | 12 | name, NUL-padded (11 characters at most) |
| 20 | 4 | catalog number |
| 24 | 4 | uplink, Hz |
| 28 | 4 | downlink, Hz |
| 32 | 2 | uplink CTCSS, Hz x 10, 0 none |
| 34 | 2 | arming CTCSS, Hz x 10, 0 no arming |
| 36 | 2 | arming carrier, ms |
| 38 | 2 | awake time after arming, s |
| 40 | 64 | the elements: epoch (Unix s), B*, inclination, node, argument of perigee, mean anomaly (degrees), eccentricity, mean motion (revolutions a day) |
| 104 | 20 | 0 |
| 124 | 4 | CRC-32 |

## 7. Cost

Measured on a Cortex-M4 build without its floating-point unit (`-mfloat-abi=soft`, `-Os`, LTO, newlib nano), every
function in use, against an empty program:

| Item | Flash | Notes |
|---|---:|---|
| `core/sat` and what it pulls from the C library | 17,200 B | 26,976 B with the C library's `pow`, `asin`, `fmod`, `sin` and `cos` (3.2) |
| of which software double arithmetic | about 2 KB | Add, multiply, divide; shared with any other double code in the image |
| of which `atan2`, `atan`, `sqrt` | about 1.7 KB | |
| Static RAM | 12 B | The UTC clock. The caller owns `sgp4_t` (376 B), `sat_observer_t` (88 B), `sat_elements_t` (64 B) per stored satellite, `sat_pass_t` (32 B) per listed pass, `sat_arm_t` (16 B) |
| Stack | about 0.75 KB | `sat_pass_next` down to the soft-float helpers, with `-fstack-usage`; the task that runs it needs that headroom |

In a firmware image (an STM32F405 radio's default build, LTO, 2026-11-03), `CONFIG_SAT=y` with the debug link adds
26,792 B of flash and 2,848 B of static RAM, and the `sat` task's 1,536-byte stack from the RTOS heap: the library, the
store, the service and its task (5.1), the screens, the SAT command and its tracker. The static stack check finds the
`sat` task 88 bytes clear and no other task deeper than without it: the debug link's view shows the last LOOK rather
than running the model under the status page, and the store's scans read a slot in 32-byte pieces.

Measured on a CK803S (168 MHz, no FPU, 2026-11-05, `sat bench` over the debug link): **3.76 ms a run**
(67 runs in 251,666 us). One update a second for live tracking is under 0.4% of the processor; a pass search of at most
about 90 runs is 0.3 s, and a day of passes for one satellite (about 1,600 runs) is about 6 s of processor time, which
the service spreads over slices at the idle priority (5.1). A host build of the same code takes about 1 us a run, and
an ARM Cortex-M4 with its FPU 13 us.

## 8. Testing

`tests/host/satTest.c` (`make -C tests/host`):

| What | Against | Agreement |
|---|---|---|
| TLE parsing, check digits, Alpha-5, epochs, errors | hand-worked values | exact |
| `sat_sincos` | the C library | 2.2e-16 over +-5000 rad |
| SGP4 states, five element sets (ISS, SO-50, AO-91, a perigee under 220 km, an Alpha-5 number), -1 to +3 days | python-sgp4 | under 1e-9 km, 1e-12 km/s |
| Look angles, 618 samples over a day, above and below the horizon | Skyfield | 0.004 degree, 0.04 km, 0.3 m/s |
| 41 passes over two days | Skyfield's rise / culmination / set | 0.6 s at rise and set, 0.3 s at the peak, 0.003 degree |
| Deep-space refusal, decay, minimum elevation, a pass in progress | constructed cases | |
| Doppler, locators, the arming state machine and the UTC clock, each with the millisecond clock's wrap | hand-worked values | exact |

`tests/host/satLinkTest.c` drives the debug link's SAT command and `sat` view section the same way, on a fake clock
and a simulated flash: the element set in pieces, a typo the check digit catches, looks and passes against the same
reference values, the bench, saving, listing, recalling, deleting and clearing, and the refusals.
`tests/host/satStoreTest.c` covers the store on the simulated flash: records in and out bit for bit, replace, full,
delete, foreign bytes, maps that give no area, the shared buffer taken, and a power cut at every unit of a write (the
old record or the new one after the next boot, never a mix). `tests/hil/test_sat.py` does it on the radio, reading the reference values from
`tests/host/sat_ref.h`, records the time a run, and stores, recalls and deletes a made-up satellite.

`tools/sat_reference.py` writes the reference values into `tests/host/sat_ref.h`, which is committed so the tests
don't need Python. It runs python-sgp4 and Skyfield (both MIT) as black boxes; regenerate after changing its element
sets (`pip install sgp4 skyfield`).

## 9. Not written yet

| Piece | Needs |
|---|---|
| A real-time clock on other boards | `hal_rtc_read` / `hal_rtc_write` (4.1.1) where the hardware has one. The one written is untested on a radio |
| Getting element sets in | Keypad entry with the check digits, and data over audio. The debug link loads and stores them already |
| Satellite areas on other boards | A `sat_addr` / `sat_size` in each board's flash map where it has room |
| Tracking, the rest | The arming countdown on the screen and the arming carrier keyed through the radio core; Doppler applied to receive and transmit through the core (the screen only shows the corrected frequencies); a short way in (a programmable key) |
| Editing profiles on the radio | Frequencies and tones of a stored satellite; for now they come with SAVE over the debug link |
| Speed on other radios | `sat bench` or `tests/hil/test_sat.py` on each radio: the time per model run (7). The CK803S's is measured (7) |
