<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 The RadioFoundry Authors -->

# DM-32UV: loading element sets by sound

Goal: update the satellite element sets (TLEs) by playing a sound from a phone at the radio's microphone, with no
cable. It is the route for a phone that can't use the programming cable (an iPhone has no Web Serial), and it needs
nothing but a speaker and a web page. This is the plan and its first experiment. Nothing here transmits. The wider plan
is [dm32-satellite-plan.md](dm32-satellite-plan.md) (phase 8, "sound transfer").

Status: **phase 0, the mic probe, first run on the radio 2026-11-12.** Samples flow with the transmitter off (`n` counts
up, `on`), so the mic PCM path itself runs. The level stayed at 0 with a phone tone at the mic: the mic wasn't reaching
the codec. The code explains it: outside transmit the mic switch (GPIO 0x2E) follows the speaker source and sits on the
**analog FM path** whenever the squelch is closed (`speaker_source` in `radio_dm32.c`). The second probe build holds the
switch on the codec while the meter is asked for, and adds diagnostics lines to see the switch, AUDIO_CONTROL and the
codec registers. **Second probe, same day:** `mic sw 0` (the switch is on the codec), `ac 00000012` (b4 set: mic PCM
to the CPU), `n` counting, and the level and peak still 0 with a phone tone at the mic. So the routing and the PCM tap are
as intended and the samples are still silent. The codec registers that could be read match their set-up values (AICR_DAC
and AICR_ADC C0, CR_VIC 0C, CR_CK 40, GCR_ADCL 8B, DR_ADC_AGC 34); the mic registers (0B, 0C, 0F) were cut off by the
page's width. The third build shows them, and the modem's work mode: the mic tap may run only in the DMR voice path
(work mode 0x6E), not in FM receive (0x80). A quick check needing no new build: read the same line on a DMR channel.
Nothing else is started.

## What the firmware already has

| Piece | Where | Use here |
|---|---|---|
| Bell 202 AFSK demodulator, integer only, 8 kHz, PLL clock | `core/aprs/afsk.c` | Decodes the tones. Host tested |
| HDLC framing with a CRC per frame | `core/aprs/hdlc.c` | Frames the data and drops damaged frames |
| TLE checks: 69 characters a line, check digits, same catalog number | `core/sat/sat_tle.c`, `tools/rfoundry_debug.py sat update` | The same rules decide what is stored |
| Store of up to 32 satellites | `core/sat`, `debuglink_sat.c` | The same store the cable fills |
| Mic PCM: 80 samples every 10 ms at 8 kHz in RAM, with AUDIO_CONTROL b4 | `platform/hrc7000/audio_pcm.c` | The samples to decode. A meter on them exists for VOX |

## What is not known

| Question | Why it matters |
|---|---|
| Does the mic PCM deliver samples while the radio is **not transmitting**? | The decoder needs them. The transmit path uses them, and the mic switch (GPIO 0x2E) points at the DMR path outside transmit, but nobody has read the samples ([hardware-dm32.md](spec/hardware-dm32.md) 9.0) |
| How does the codec's gain and AGC treat a phone's tones? | Clipping or squashing hurts the 1200 and 2200 Hz tones |
| How much does room noise and the phone's distance matter? | Decides the baud rate and how often each frame repeats |
| Is there RAM for a decoder next to the satellite task? | The satellite build links with about 23.7 KB free; the demodulator is small, but this is checked |

**The programming cable is in the microphone's jack and cuts the mic.** So the level can't be read over the debug link
while sound is played at the radio. Every check below is read **on the radio's screen with the cable out**. That is also
how the feature would be used: no cable.

## Phase 0: the mic probe (built)

The satellite build has `CONFIG_VOX=y` (the mic level hook for this radio) and the diagnostics page has a line:

```
mic lvl L pk P n N on|off
```

L is the level (0-100), P the peak of the last 10 ms half, N the halves measured, and on or off whether the meter runs.
Reading the line asks for the meter, so it runs with VOX off, and nothing keys. The meter stops about a second after
the page stops reading it.

**Steps** (receive only; leave the antenna on and PTT alone):

1. Back up the radio ([dm32-satellite-bringup.md](testing/dm32-satellite-bringup.md) section 2), then install the
   satellite build from this branch's pre-release.
2. Leave VOX off in Settings. Pick an FM channel (the meter runs in the state VOX would run it in). Take the
   programming cable **out**.
3. **OK**, **6**, **7** (Tools > Diagnostics). Find the `mic lvl` line.
4. Quiet room: write down L, P and how N changes over 10 seconds.
5. Play a steady 1 kHz tone from a phone, the speaker 2 cm from the radio's microphone hole, at low, medium and full
   volume. Write down L and P for each.
6. Play the 1200 Hz and 2200 Hz tones the same way. Write down L and P.
7. Talk at the radio. Write down L and P.

**Reading the result**

| What you see | Means | Next |
|---|---|---|
| `on`, N grows by about 100 a second, L and P rise with the sound | The mic PCM works with the transmitter off, and the tones reach it | Phase 1 |
| `on`, N grows, L and P stay at the noise floor with any sound | Samples flow but the mic isn't connected to them outside transmit | Check the mic switch (GPIO 0x2E) and the codec ADC input in receive, then repeat |
| `off`, or N stuck | The PCM interrupt or AUDIO_CONTROL b4 isn't running in this state | The `c7 ac` register view, then repeat |
| L and P rise only at full volume, or flat-top at the same P whatever the volume | The codec clips or its AGC squashes the tones | A lower codec gain or the AGC setting, then repeat |

Also record the quiet floor and the P you get with the phone at its normal volume, so the decoder's levels can be set.

**Second build** (the mic switch held on the codec while the meter runs): the page has more lines. `mic sw S ac XXXXXXXX`
(S: 1 the analog FM path, 0 the codec; `ac`: AUDIO_CONTROL, b4 = mic PCM to the CPU) and four `cd NN ...` lines, the
codec's byte registers (CR_MIC1 0x0B, CR_MIC2 0x0C, CR_ADC 0x0F with b7 muting the mic). With the page open, `mic sw`
should read 0 and L and P should rise with sound. If they still read 0, send a photo of the page: those lines say
whether the codec's mic input, its mute bit or the ADC is the next thing to look at.

A hardware test of the same path exists: `pytest tests/hil/test_radio_features.py -k mic_level` (it needs the programming
cable, so it checks the meter runs and counts halves, not the sound).

## Phase 1: the format and the page (no radio)

- A frame format on the existing HDLC framing: one satellite a frame (its name and two TLE lines, about 150 bytes), a
  sequence number and a count, so a missing frame is known. Every frame is sent twice, with a gap. Draft; it is fixed
  after phase 0 says what the channel can carry.
- At 1200 baud a satellite is about 1.2 seconds: 32 satellites take about 40 seconds, 80 with every frame sent twice.
- The page makes the audio with WebAudio (so it works on an iPhone) and plays it, with a volume check.
- Host tests: the encoder's WAV decoded by `afsk.c`, with noise, clipping, a level change and a drifting clock added.

## Phase 2: the radio side

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
