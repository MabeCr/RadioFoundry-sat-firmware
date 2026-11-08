<!-- SPDX-License-Identifier: MIT -->

# Plan: Doppler tuning for FM satellites

Status: agreed 2026-11-07, phase 0 done 2026-11-08 (results below), phase 1 in progress. The orbit library and the Tracking screen already compute the corrected
downlink and uplink every second ([satellite.md](../spec/satellite.md) 4.3, 5.1); this plan makes the radio follow the
downlink. Decisions come from the owner's answers; open items are at the end.

## Decisions

| Topic | Decision |
|---|---|
| Scope | FM satellites only (a repeater or a beacon on FM). Linear and SSB transponders are out: the radio has no SSB receiver |
| What Tracking tunes | The satellite's profile in the temporary VFO: the downlink as receive, the uplink as a split transmit, 25 kHz wide, the uplink's tone as the transmit tone, no receive tone. Back puts the channel that was on view back |
| Opening Tracking | Changes nothing. The first `#` enters the VFO state and tunes, so looking at a pass never changes what the radio is playing |
| Manual correction (first) | `#` retunes the receiver to the downlink as it should be at that moment |
| Automatic correction (second) | `*` turns it on and off. It retunes when the needed frequency differs from the tuned one by a threshold, with a minimum interval, preferring a gap in the signal (squelch closed); the numbers come from the measurement below |
| Before the pass | The shift for the rise time is applied from about a minute before it |
| At the set | The radio stays tuned until Back |
| Transmit | The uplink is left at its nominal frequency, with its tone: a cross-band split the VFO allows (`radio_req_vfo_tx` checks only that the frequency is in a transmit band). No Doppler on it: not needed on FM at these shifts, a nice-to-have later. Transmit is never retuned, and no retune happens during one |
| Position | The satellite task keeps the last position it saw: after a fix is lost it goes on with that one. Time is required: without it nothing is tuned |
| Display | The Tracking screen's receive line shows the tuned frequency and, when the radio is behind the needed one, by how much |

## Keys on the Tracking screen

Decided 2026-11-08. Only keys the screen has no other use for; the radio's own keys keep working (Monitor held opens
the squelch while held, PTT, the volume).

| Key | Does | Notes |
|---|---|---|
| `#` | Retune to the downlink as it should be now (phase 1) | The first press enters the temporary VFO |
| `*` | Automatic correction on and off (phase 2) | |
| `0` | Squelch open on and off, latched | Listening to a whole pass with the squelch open: no key held. The strip shows MON (as for the monitor today) and a toast says "Squelch open" or "Squelch normal". Left again by Back, so the channel that comes back keeps its squelch. Needs a latched monitor request in the radio core (the monitor key today is a hold) |
| Dial (the encoder) | Nudges the downlink by a step of 1 kHz a detent, remembered for the pass | The computed Doppler can be right while the satellite's own transmitter sits slightly off, so people tune by ear. The nudge is added to every corrected frequency, manual or automatic, and shows in the receive line ("+1.0k tuned"). Pressing the dial, if the radio has that key, clears it. Step size to settle after phase 0 |

How the screen tells the user about them: the hint row lists them with shorter forms for narrow screens ("0 Squelch  # Retune  * Auto"
on a tall panel, fewer words on a small one, as other screens do with their text alternates), a toast confirms each
toggle, and the status line carries MON. No modal prompt.

With the squelch latched open there is no gap in the signal to retune in, so the automatic mode (phase 2) must not wait
for one then: it retunes on the threshold and the minimum interval alone, and the measurement from phase 0 says
whether the audio click of a retune is acceptable. Until then `*` with the squelch open is the open question below.

## Phases, with a gate between each

| Phase | Work | Gate |
|---|---|---|
| 0. Measure | A retune test in the firmware: the receiver is stepped between a base frequency and an offset of it a number of times, and the time the signal takes to settle after each retune is timed on the radio's own clock. The base frequency is the user's to set (a weather channel such as 162.400 MHz is a steady FM carrier; the page offers it as the default). A debug-link command starts it and reads the result; the satellite page has a button | Numbers from a real radio: how long a retune takes, whether the signal drops, and how that grows with the step |
| 1. Manual | The core request for a light retune (one `rx()` call, no end of reception, no change to the channel record); the temporary VFO entered by `#` and left by Back; the last-position change; host tests and the simulator | Host tests, flows, a bench check with the weather channel |
| 2. Automatic | `*`, the threshold and the gap logic, with the numbers from phase 0 | A weather-channel replay of a pass's shifts, then a real pass |
| 3. A real pass | With the yagi, on the ISS APRS downlink (a steady beacon) and an FM repeater satellite | The pass decodes and keeps the squelch open |
| Later | The SO-50 arming burst through the radio core, Doppler on the uplink | |

## The retune test (phase 0)

Receive only: it never transmits. The radio must be idle, on an FM channel or the VFO (a DMR channel refuses it), and
the base frequency in a band it receives. The radio sets the base frequency itself, so no channel for it needs storing.

| Parameter | Meaning |
|---|---|
| base | the frequency to hold between hops, Hz (the page: MHz, default 162.400) |
| step | the offset of the second frequency, Hz, signed (1 to 10 kHz covers a pass) |
| count | the number of retunes, 1 to 32, alternating base and base + step |
| gap | milliseconds between retunes (20 to 1000) |
| width | 12.5 or 25 kHz (25) |

For each retune: the time the `rx()` call itself took, the time from the call until the signal level is back within
3 dB of the level held on the base frequency (for three reads in a row), the lowest level seen meanwhile and the
highest noise reading. The channel the radio was on is tuned again when it ends. Reading the settle time needs a steady carrier;
without one the test still reports the call times.

## Phase 0 results (2026-11-08, 162.400 MHz weather carrier, 25 kHz wide, 12 retunes 150 ms apart)

| Step | `rx()` call | Settle (median, range) | Steady level base / other | During the retune |
|---|---|---|---|---|
| 2 kHz | 1.46 ms | 10.7 ms (9.7 to 11.7) | -75 / -75 dBm | level at the floor (-148 dBm), noise 137 against 60 steady, 20 to 24 reads (200 µs apart) with no carrier, in 12 of 12 |
| 10 kHz | 1.46 ms | 9.3 ms (7.5 to 10.2) | -71 / -92 dBm | the same: floor level, noise 137, 15 to 21 reads, in 12 of 12 |

What it means:

- Every retune re-locks the synthesizer: about 1.5 ms in the call, about 10 ms until the level is steady, of which about 4 ms
  with no carrier. It doesn't depend on the step (2 and 10 kHz settle alike), so there is no cheaper small step.
- A frequency error costs signal: 10 kHz off is 21 dB down, 2 kHz is no loss. The receiver tolerates about 2 kHz, so the
  automatic mode retunes before the error reaches it.
- 10 ms about every 30 to 60 seconds (estimated from a pass's Doppler rate on 70 cm, less often on 2 m) is a click, not a
  gap: no need to wait for a gap in the signal, and a latched-open squelch is no obstacle.
- Not measured: whether the audio mutes or plays a burst of noise (the cable mutes the speaker on the test radio, or at
  least nothing was heard), so the interval below is set from the level only.

Proposed numbers for phase 2, to adjust after a real pass: retune when the needed frequency differs from the tuned one by
1.5 kHz or more; at most once every 5 seconds; no wait for a gap.

## Open items

- Whether the audio drops or clicks during a retune (listen with a second radio nearby).
- Whether the synthesizer's swing between bands at key-up (the split) is quick enough for a satellite's key-up: a transmit
  test, with a dummy load, after phase 1.
- The automatic mode's threshold and interval (proposed above), from a real pass.
- The dial: that it can be read on the Tracking screen without stepping a list, and whether the radio has a press on it.
