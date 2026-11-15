<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 The RadioFoundry Authors -->

# Element sets by sound

A phone plays a sound at a radio's microphone and the radio stores the satellite element sets (TLEs) it carries. No cable
is involved, and an iPhone can do it, which has no Web Serial. This page is the signal and the frame. Which radios can
hear it: [Feature matrix](../feature-matrix.md).

Status: the frame, the sender, the receiving logic and the radio's screen are built (`CONFIG_SAT_SOUND`). The receiving logic and the screen are host tested; **tried on a radio (2026-11-12): a phone playing the page's sound at the microphone loaded satellites**. Tuning (volume, noisy rooms, repeat count) is not done.

## The signal

| Part | Value |
|---|---|
| Modulation | Bell 202 AFSK, 1200 baud, mark 1200 Hz (a 1), space 2200 Hz (a 0), phase continuous ([aprs.md](aprs.md) 4) |
| Framing | HDLC: flags (0x7E), bit stuffing, the 16-bit FCS, NRZI ([aprs.md](aprs.md) 4), as `core/aprs/hdlc.c` |
| Receiver | the APRS demodulator on 8 kHz samples (`core/aprs/afsk.c`) |
| Before each frame | 30 flags, about 0.2 s, for the receiver to lock |
| After each frame | 3 flags, then 100 ms of silence |
| Before the first frame | 500 ms of silence |
| Level | the sender's tone peak at about 0.6 of full scale; the receiver adapts (a tone peak of 0.0015 of the microphone ADC's full scale, 18 dB over a quiet room's floor, is enough on the radio it was measured on) |

## The frame

One satellite is one HDLC frame. The FCS is the HDLC one, so the frame itself has no checksum:

| Byte | Field | Value |
|---|---|---|
| 0 | magic | 0x54 (`T`) |
| 1 | version | 1, or 2 when the frame ends with a profile; a receiver ignores a frame of another version |
| 2 | transfer id | 0-255, the same for every frame of one transfer |
| 3 | index | 0 to count - 1: which satellite of the transfer this is |
| 4 | count | 1 to 32, the satellites in the transfer (the store holds 32) |
| 5 | name length | 0 to 24 |
| 6 | name | printable ASCII, no terminator |
| then | line 1 | the TLE's first line, 69 characters |
| then | line 2 | the TLE's second line, 69 characters |
| then (version 2) | profile | 16 bytes, little endian: uplink Hz (4), downlink Hz (4), uplink CTCSS tone in Hz x 10 (2), arming carrier tone in Hz x 10 (2), arming carrier time in ms (2), the repeater's awake time in s (2) |

The profile is what picking the satellite sets ([satellite.md](satellite.md) 6): the fields of `sat_profile_t`. A frequency is 0 (none) or
100 MHz to 1.3 GHz, a tone 0 or 60.0 to 260.0 Hz, the arming carrier at most 10 s and the awake time at most an hour; a frame with a
value out of range is refused whole. A sender with no profile for a satellite sends a version 1 frame (an all zero profile is one too).

A version 1 frame is at most 168 bytes, a version 2 frame at most 184, about 1.4 s of audio with its flags and gap (1.7 s with the longest name). The receiver refuses a frame whose bytes are not
exactly this length, whose lines fail the TLE rules (69 columns, line numbers 1 and 2, both check digits, the same catalog
number), whose name is longer than 24 or holds a character outside 0x20 to 0x7E, or whose index is not below the count.
`core/sat/sat_sound.c` is the reference.

## A transfer

The sender plays every satellite's frame in index order, then the whole list again (two passes, with a 1 s gap between the
passes). Every frame stands alone: the radio stores each one that passes the checks and the rules of an update (the epoch is
later than the stored set's), as the debug link's `sat update` does ([debuglink.md](debuglink.md) 3.24). A frame lost to
noise is lost only until the next pass. The transfer id and the count tell the receiver which transfer it is hearing: a new
id, or a new count, starts the tally of "n of m" again.

32 satellites take about 45 s a pass (55 s with the longest names), 90 s with the repeat: the reference sender's file for 32
short-named sets is 87 s long.

## The radio

With `CONFIG_SAT_SOUND` the last row of Tools > Satellites is **Receive sound** (`sat-sound`, [ui.md](ui.md) 2.6). Opening it asks
the radio core to listen (`RREQ_SOUND_RX`, `radio_req_sound`); closing it stops. While it listens:

| Part | What |
|---|---|
| Samples | the PCM stream's input, 8 kHz in 20 ms blocks, through the audio mux's receive ring (`audio_mux_sound_rx`, [usb-audio.md](usb-audio.md) 5.1). On the radio this was measured on, the microphone reaches that input only in the modem's DMR work mode, so the radio must be on a **DMR channel**; the screen says so otherwise |
| Mic route | a board whose idle microphone switch points away from the codec keeps it there while the core listens (`hal_radio_t.mic_listen`, optional) |
| Not while | transmitting (PTT ends it), in a hotspot, or when the satellite store is missing or someone else's |
| Decoding | in the radio task: `core/sat/sat_sound_rcv.c` feeds the AFSK demodulator, checks each frame (`sat_sound.c`) and tallies the transfer |
| Storing | each satellite heard waits in a queue of four for the housekeeping task, which stores it (`storage_job_post`, `sat_sound_apply.c`), so the radio task never waits on the flash. The rules are those of an update: a new satellite is stored (with the profile its frame carries, else the built-in one if the radio knows the number, else its name and no frequencies); a stored one is replaced only by a later set and keeps its profile, except that a profile the frame carries fills in one that has no frequencies at all (a satellite loaded by name only) and never replaces one that has any: what you set stays; the same set or an older one is kept out; a full store, an orbit the model refuses and a store failure are counted |
| A full queue | a satellite that finds the queue full is forgotten, so its copy in the sender's next pass is taken |
| Shown | the loudest sample of the last block as a bar; before the first satellite a line of the receiver's own numbers, "Lvl 450 Fr 2 Err 1" (the level, frames decoded, frames that failed a check) to tell no sound from sound the decoder can't read; then "Sats heard n of m", new, updated, kept (same or older) and problems |
| Debug link | the `sat` view's `sat.sound.*` keys ([debuglink.md](debuglink.md) 3.24) |

The listening ends when the screen is left. Satellites already heard still go to the store.

## The reference sender and the shared vectors

`tools/sat_sound.py amateur.txt -o sets.wav` makes the audio from a TLE file (CelesTrak or AMSAT, 3-line or 2-line sets). The
same file writes `tests/data/sat_sound_vectors.json`: frames and their HDLC tone bits that the C code
(`tests/host/satSoundTest.c`) and the satellite element set page's encoder
([sat-web-tool](../dev/sat-tle-web-tool.md)) must reproduce byte for byte. `tools/ci.sh` fails if the file differs from what
the Python sender gives now.

## What the host tests show

Through the audio path (a 48 kHz sender, a codec's average-and-drop to 8 kHz, noise, clipping, hum, a DC offset) all 32
satellites arrive in two passes down to a tone 15 dB over the noise, clipped hard, with a 50 Hz hum 6 dB under the tones, and
with the sender's clock 0.5 % off. At 10 dB over the noise 62 of 64 frames decode; at 6 dB none do (white noise over the
whole 8 kHz band): the demodulator's edge is between the two.
