<!-- SPDX-License-Identifier: MIT -->

# Plan: Doppler tuning for FM satellites

Status: agreed 2026-11-07, phase 0 in progress. The orbit library and the Tracking screen already compute the corrected
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

## Open items

- How long a retune takes on this hardware, and whether the audio drops: the test answers it.
- Whether the synthesizer's swing between bands at key-up (the split) is quick enough for a satellite's key-up: a transmit
  test, with a dummy load, after phase 1.
- The automatic mode's threshold, interval and gap rule, and what it does with the squelch latched open.
- The dial: that it can be read on the Tracking screen without stepping a list, and whether the radio has a press on it.
