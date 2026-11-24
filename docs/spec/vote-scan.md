<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 The RadioFoundry Authors -->

# Vote scan

**Status: built, untested on a radio.** The record, the engine, the radio core and the screens are written and host
tested; peeks through the radio core are not (section 8). Section 9 lists what is still open.

Vote scan keeps the radio on the best site of a conventional system whose sites all carry the same traffic on
different frequencies: P25 first (the case that asked for it), and FM, DMR or NXDN the same way. A normal scan stops on the first channel with activity; vote scan measures every site and
listens to the strongest, moving when another becomes clearly better. The user picks one "channel" (the vote list) and
the radio does the site selection.

Switch: `CONFIG_VOTE_SCAN`, which needs `CONFIG_SCAN_LIST` ([build-config.md](../build-config.md)). It builds on the
stored scan lists ([scan-list.md](scan-list.md)) and the scan engine (`core/radio/scan.c`), and on each mode's own
receiver: P25 ([p25.md](p25.md)), DMR, FM and NXDN. For DMR, repeater roaming ([dmr-roaming.md](dmr-roaming.md)) already
picks the best site of a zone with beacons and a survey; vote scan is the simpler choice for a fixed list.

| Section | Contents |
|---|---|
| 1 | The system it serves |
| 2 | The vote list and its settings |
| 3 | The record |
| 4 | Voting between calls and at the start of a call |
| 5 | Voting during a call |
| 6 | Transmit |
| 7 | What the user sees |
| 8 | The debug link and tests |
| 9 | Open questions |

## 1. The system it serves

| Property | Assumed |
|---|---|
| Sites | 2 to 16 repeaters, each on its own output frequency |
| Traffic | every site repeats the same calls (a linked or simulcast-like network) |
| Identity | usually the same NAC and talk group (P25), color code and talk group (DMR), RAN (NXDN) or tone (FM) on every site; not required (each member keeps its own) |
| Inputs | a site may hear only its own input, or every site's input (then transmit steering is optional) |
| Site-only traffic | talk groups that stay on one site are ordinary channels, not vote list members |

A system that already votes the uplink (its repeaters compare what each heard) needs nothing more from the radio than a
good downlink choice. One whose repeaters each hear only their own input needs transmit steering (section 6).

## 2. The vote list

A vote list is a stored scan list with its Vote flag set. The list editor on the radio (Tools > Scan, [ui.md](ui.md)
2.7) gets a **Vote** row in the list's options; the CPS has no field for it, so the radio is where it is set.

| Rule | Why |
|---|---|
| Every member has the same mode: all FM, all DMR, all P25 or all NXDN | the members carry the same traffic, so one system, one mode; signal readings compare like with like |
| Turning Vote on refuses a list that mixes modes: "Vote: one mode only" | the user removes the odd ones first; nothing is dropped silently |
| While Vote is on, the channel picker offers only channels of the list's mode | the same rule at the point of entry |
| A trunked channel (`CODEPLUG_KIND_P25_TRUNK`, `CODEPLUG_KIND_DMR_TRUNK`) is never a member | a trunked system picks its own site |
| At least 2 members | one site has nothing to vote between |
| The first member is the start site | the radio starts there and, with fixed transmit (section 6), transmits there |

A channel the codeplug no longer holds stays in the list and is skipped, as in a scan list.

## 3. The record

The scan list record ([scan-list.md](scan-list.md) 3) keeps its layout byte (1). Vote uses its reserved flags byte and
the first reserved bytes; a firmware without vote scan reads the same record as a plain scan list.

| Offset | Size | Field | Default (0 reads as) |
|---|---|---|---|
| 5 | 1 | flags: bit 0 Vote, bit 1 peek during calls off, bit 2 fixed transmit (else steering) | 0: plain list |
| 184 | 1 | threshold, dBm as i8; 0 = default | -90 dBm |
| 185 | 1 | hysteresis, dB; 0 = default | 6 dB |
| 186 | 1 | fixed transmit member index (0-based) when flags bit 2 is set | 0: the first member |
| 187 | 1 | reserved, 0 | |

A record from a firmware that set none of these reads with every default: Vote off.

Every value is the list's own: two vote lists for two systems keep their own threshold, hysteresis, peeks and transmit
choice. The list's options screen (Tools > Scan, a list, then its options) shows them once Vote is on:

| Row | Values | Default |
|---|---|---|
| Vote | On, Off | Off |
| Threshold | -50 to -120 dBm, in 5 dB steps on the radio (the record takes -120 to -50 in 1 dB steps) | -90 dBm |
| Margin (hysteresis) | 2, 3, 4, 5, 6, 8, 10, 12, 15 or 20 dB on the radio (the record takes 1 to 20) | 6 dB |
| Peek during calls | On, Off | On |
| Transmit | Voted site, or one member (the picker lists them) | Voted site |

## 4. Voting between calls and at the start of a call

A conventional repeater transmits only while there is a call, and the sites of a linked system carry each call at the
same moment. Between calls every site is silent, so there is nothing to compare: the vote happens when a call is on
the air. The engine is `core/radio/vote.c` (no hardware; `tests/host/voteTest.c` drives it against a simulated system).

| Phase | What happens |
|---|---|
| HOLD | on the voted site between calls. Kept while that site was strong in its last call (at or above the threshold): the next call is heard there at once, nothing lost. After `VOTE_RECHECK_MS` (30 s) with no call, one SWEEP in case a call is on the air that this site doesn't carry to us |
| SWEEP | the voted site weak or not known yet: each member in turn for `VOTE_SWEEP_DWELL_MS` (150 ms), listening for a call, as a scan does. On a digital member (DMR, P25, NXDN) a carrier, read by the strength at the squelch level after the settle, makes the sweep wait up to `VOTE_PROBE_MS` (700 ms) from the retune for the member's sync: a P25 NAC comes once a 180 ms frame, so the dwell alone misses most calls. A member whose carrier gave no sync (another system on the frequency) is not waited on again for `VOTE_PROBE_BACKOFF_MS` (5 s). FM needs none: its squelch is the call |
| ROUND | a call heard on a member not known to be strong: each other member for `VOTE_SAMPLE_MS` (100 ms, the 50 ms settle included), then the strongest is voted if it beats the member the call was heard on by the margin. The round costs the start of that call (about 100 ms a member) |
| CALL | on the voted site while the call lasts: its reading averaged (1/8 a poll); section 5 for peeks. `VOTE_HANG_MS` (1 s) after the call ends, the average is kept as the site's reading and the radio is in HOLD |

A member counts as hearing a call only with its own mode's sync: its NAC (P25), color code (DMR), RAN (NXDN) or tone
(FM, when the channel has one); a carrier without it is another system. A member that reads nothing is weaker than any
that does.

## 5. Voting during a call

A radio with one receiver (`board.rx2` NULL) can measure another site only by leaving the current one, which loses the
voice frames for that moment. The design keeps that rare and short:

| Rule | Value |
|---|---|
| No peeks while the voted site's average is at or above the threshold | nothing is lost on a good site |
| Below it, one peek at a time, at most every `VOTE_PEEK_GAP_MS` (2000 ms) | a call loses at most one short gap every two seconds |
| A peek visits one member (round robin) for `VOTE_PEEK_MS` (60 ms: a 40 ms settle, then readings) | about three IMBE frames |
| A peeked member that is better stays tuned: the call carries on from it with no second retune | the switch costs nothing more |
| Where the radio core can tell, a peek starts at the mode's frame boundary: after a P25 LDU, in a DMR repeater's other timeslot where it fits, after an NXDN frame; FM has none (not in the first version: measure first) | the loss falls on whole frames |
| Peeks are off when the list's "peek during calls off" flag is set | the safety net the owner asked for: voting then happens only at the start of a call (section 4) |

The P25 engine plays a lost frame as its own repeat or silence ([p25.md](p25.md)); the length of the gap a peek costs
is the first thing to measure on a radio (section 9). On FM a peek is a short hole in the audio, so an FM list starts with
Peek during calls off; the user can turn it on.

## 6. Transmit

| Mode | PTT transmits on |
|---|---|
| Steering (default) | the voted site's own channel: its TX frequency, NAC and talk group |
| Fixed | the member named by the fixed transmit index, whatever site is voted |

PTT during a search transmits on the voted site (steering) or the fixed member; the search stops and starts again after
the transmission. Transmit checks (TX inhibit, band, RX only) apply to the member used, as on any channel.

## 7. What the user sees

| Where | What |
|---|---|
| Status pill | **VOTE** while a vote list runs and nothing is heard, RX on a call, as SCAN does for a scan. Its color is the theme's private-call color (`ui_col.priv`): cyan or teal in most themes, blue in the light ones, red in Night, so it reads as its own state beside SCAN's amber |
| Home | the vote list's name on the zone line with the voted member's place ("2/3"), and the voted site's channel name as the channel, without a number (the tuned channel changes under a sweep or a peek); the hint "Back stop vote" |
| Activity | the call as on any P25 channel (caller, talk group) |
| Tools > Scan | the list shows "Vote" beside its channel count; OK starts it ("Voting " and its name) |
| Back | stops voting and stays on the voted site, as Back ends a scan ("Vote stopped") |

The screens are in [ui.md](ui.md) 2.7.1.

## 8. The debug link and tests

The `roam` view section ([debuglink.md](debuglink.md) 3.7) ends with the vote keys, as roaming and voting both choose
between sites; `features` says `scan=vote` while a list votes. The keys are printed only then, so the section still fits a
reply:

| Key | Meaning |
|---|---|
| `vote.built` | 1 with `CONFIG_VOTE_SCAN` |
| `vote.phase` | hold, sweep, round, call or peek (section 4) |
| `vote.n`, `vote.voted`, `vote.at` | members, the voted member (0-based) and the one tuned now |
| `vote.avg_dbm` | during a call, the voted site's average |
| `vote.rounds`, `vote.sweeps`, `vote.switches`, `vote.peeks` | counters since the list started |
| `vote.peek_max_ms` | the longest peek, the retune back included |
| `vote.dbm` | each member's last reading, comma separated, `-` before one is read |
| `vote.site` | the voted member's channel name |

Host tests: `tests/host/voteTest.c` drives the engine against a simulated system (first call, a strong site holding, the
margin, peeks and peeks off, a site out of range); `tests/host/radioCoreTest.c` (`test_p25_vote`) runs a vote list of
three P25 sites in three zones through the radio core (the round, PTT on the voted site and on a fixed member, a stop, a
list mixing modes refused, the debug link keys). UI flows (`11 vote: ...` in `tests/host/uiFlowTest.c`) set each option, check the refusals, the picker's mode rule,
the start and the stop. Still to write: DMR and FM vote lists through the radio core; a carrier without the member's
sync reading as none; peeks through the radio core with their gap measured.

## 9. Open questions

| Question | How to answer it |
|---|---|
| How long does a peek really cut the audio on a one-receiver radio? | the retune test on the debug link, then a peek counter and `vote.peek_gap_ms` on the radio |
| Is the -90 dBm default right for this radio's signal reading? | compare `vote.rssi.<n>` with the sites' known coverage on a drive |
| Should decode quality (bit errors per voice frame) break ties? | log both during a drive and compare |
| Do members ever need different NACs or talk groups? | allowed by the design; no system needing it is known yet |
| Does a DMR peek fit in a repeater's other timeslot? | measure the retune and reading time first; if it fits, a DMR call loses nothing |
| Is the private-call color right for VOTE in every theme? | the gallery renders it in each theme |
