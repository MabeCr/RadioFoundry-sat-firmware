<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 The RadioFoundry Authors -->

# DM-32UV: loading element sets by sound

Goal: update the satellite element sets (TLEs) by playing a sound from a phone at the radio's microphone, with no
cable. It is the route for a phone that can't use the programming cable (an iPhone has no Web Serial), and it needs
nothing but a speaker and a web page. Nothing here transmits. The wider plan is
[dm32-satellite-plan.md](dm32-satellite-plan.md) (phase 8, "sound transfer").

Status: **phase 0, the mic probe, is done (2026-11-12): the mic can be read with the transmitter off, in the modem's DMR
work mode, and the tones come through at a usable level.** The probe's firmware was temporary and is not in the tree
(see phase 0). Step 1 is built: the signal, the frame, the reference sender and the host tests are in
[sound-transfer.md](spec/sound-transfer.md), `core/sat/sat_sound.c`, `tools/sat_sound.py` and `tests/host/satSoundTest.c`; the page's
encoder is in the page's repository. Step 2 is built too: the radio's **Receive sound** screen (`CONFIG_SAT_SOUND`, on in the
satellite build), host tested. **No sound has been played at the radio yet.** The first build heard nothing on a DMR channel
because the mic switch was not held on the codec; the radio core now holds it while it listens (see
[hardware-dm32.md](spec/hardware-dm32.md) 9.0).

## What the firmware already has

| Piece | Where | Use here |
|---|---|---|
| Bell 202 AFSK demodulator, integer only, 8 kHz, PLL clock | `core/aprs/afsk.c` | Decodes the tones. Host tested |
| HDLC framing with a CRC per frame | `core/aprs/hdlc.c` | Frames the data and drops damaged frames |
| TLE checks: 69 characters a line, check digits, same catalog number | `core/sat/sat_tle.c`, `tools/rfoundry_debug.py sat update` | The same rules decide what is stored |
| Store of up to 32 satellites | `core/sat`, `debuglink_sat.c` | The same store the cable fills |
| Mic PCM: 80 samples every 10 ms at 8 kHz in RAM, with AUDIO_CONTROL b4 | `platform/hrc7000/audio_pcm.c` | The samples to decode. A meter on them exists for VOX |

## What was not known, and what the probe found

| Question | Answer |
|---|---|
| Does the mic PCM deliver samples while the radio is **not transmitting**? | Yes in the modem's DMR work mode (0x6E). In FM receive (0x80) the samples flow but are all zeros |
| How does the codec's gain and AGC treat a phone's tones? | Well: the levels rise with phone volume and nothing is near clipping (table below) |
| How much does room noise matter? | The quiet floor is about 8 times lower than the quietest tone tried (table below). A noisy room or a moving phone is for the tuning step |
| Is there RAM for a decoder next to the satellite task? | Not yet checked: the satellite build links with about 23 KB free; the demodulator is small |

**The programming cable is in the microphone's jack and cuts the mic**, so the level can't be read over the debug link
while sound is played at the radio. That is also how the feature is used: no cable.

## Phase 0: the mic probe (done)

Three temporary builds (the pre-releases `satellite-f6c9b2d`, `satellite-6c04388` and `satellite-0694445`, from commits
of the branch `feat/mic-probe`) wired the VOX mic level meter on the mic PCM (`platform/hrc7000/audio_pcm.c`) and
showed it, the mic switch, AUDIO_CONTROL, the modem's work mode and the codec's registers on the diagnostics page, read
on the radio's screen with the cable out. What they found, in order:

1. **FM channel:** `mic lvl 0 pk 0` with `n` counting up (about 100 halves a second). The mic PCM tap runs, the samples are
   zeros.
2. **Why, first guess:** outside transmit the mic switch (GPIO 0x2E) follows the speaker source and sits on the analog FM
   path while the FM squelch is closed (`speaker_source` in `radio_dm32.c`). A build held the switch on the codec while the
   meter ran: `mic sw 0`, `ac 00000012` (AUDIO_CONTROL b4 = mic PCM to the CPU), still zeros. The codec registers that
   were readable matched their set-up values.
3. **DMR channel, no transmission:** L and P jump with sound. The mic PCM tap runs in the DMR work mode, not the FM one.
   The mic switch hold and the DMR mode changed together in that test, so whether the hold is needed in the DMR mode was
   not tested on its own.

The firmware was removed from the branch afterward: it existed only to take these readings. It is in the branch's
history, and `git log feat/mic-probe` finds it.

## Phase 0 result: tones through the mic (2026-11-12, DMR channel, no transmission, phone speaker at the mic hole)

| Tone | Phone volume | L | P |
|---|---|---|---|
| 1200 Hz | low | 20 | 50 |
| 1200 Hz | medium | 38 | 210 |
| 1200 Hz | high | 48 | 450 |
| 2200 Hz | low | 25 | 65 |
| 2200 Hz | medium | 46 | 360 |
| 2200 Hz | high | not tried (a sleeping baby) | |

P is on a 0 to 32767 scale, so even the highest reading, 450, is about 1.4 % of full scale (about -37 dBFS): nothing is
near clipping, and the readings keep rising with volume, so the codec's gain control isn't flattening the tones. The
two tones read alike (2200 Hz a little higher at the same volume), which the demodulator's per-tone normalization
absorbs. The quiet floor in the DMR mode, in a quiet room with no sound, reads L 0 and P 4 to 8. So the lowest volume
tried (P 50 to 65) is about 8 times the floor (18 dB), and medium (P 210 to 360) about 40 times (32 to 35 dB): a quiet
phone is enough, and the codec's mic gain (a set-up value, 11 dB) needs no raising. What this doesn't say: how
a noisy room or a moving phone changes it.

## Phase 1: the format and the page (no radio)

- A frame format on the existing HDLC framing: one satellite a frame (its name and two TLE lines, about 150 bytes), a
  sequence number and a count, so a missing frame is known. Every frame is sent twice, with a gap (decided 2026-11-12, to
  start with; the repeat count is tuned in phase 3). The format is a draft until it is built and tested.
- At 1200 baud a satellite is about 1.2 seconds: 32 satellites take about 40 seconds, 80 with every frame sent twice.
- The page makes the audio with WebAudio (so it works on an iPhone) and plays it, with a volume check.
- Host tests: the encoder's WAV decoded by `afsk.c`, with noise, clipping, a level change and a drifting clock added.

## Phase 2: the radio side

- The Receive sound screen puts the modem in the DMR work mode while it is open (receive only), and back after. Likely the
  mic switch has to be on the codec too (the probe didn't test it alone). The codec's set-up values (mic gain 11 dB) are
  enough: no new gain.
- The mic samples are the 10 ms PCM halves of `audio_pcm.c`; a decoder takes them in the same task that runs the vocoder
  callback, or a ring read by the satellite task.
- Tools > Satellites > **Receive sound**: a level meter, the frames heard, the satellites stored. It listens only while
  the screen is open.
- Every frame goes through the same checks as `sat update`: a bad line, a different catalog number, or an older epoch is
  dropped. Only satellite records are touched. Nothing else can be written by sound.
- Host tests with the page's WAV files.

## Phase 3: on the radio

Tune with a real phone: the baud rate (1200 or lower), the repeat count, the level meter's guidance. If the room or the
phone is the weak link, a lower baud rate and more repeats are the fallback.

## Not planned

- A background listener: sound is only taken while the Receive sound screen is open.
- Anything over the air: the radio's receiver is not used, only its microphone.
- Writing anything except satellite records.
