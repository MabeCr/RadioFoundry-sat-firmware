<!-- SPDX-License-Identifier: MIT -->

# Plan: a web page to update satellite element sets and run diagnostics

Status: steps 0 to 5 are done (step 0 on the owner's phone: the port chooser lists the cable and the radio answers);
the page is in `tools/sat-web/` ([README](../../tools/sat-web/README.md)). Left: step 6 (host it and try an update from
the phone) and the rest of step 7. This is the owner's own page, in its own folder of this repository (`tools/sat-web/`,
proposed), published on a static host the owner controls. It is not part of the RadioFoundry site and shares none of its
build. The goal is a page that runs in a phone's browser with the programming cable on an
OTG adapter: keep the radio's satellite element sets (TLEs) current, set its clock from the phone, and show the
diagnostics that today need the host tool ([debug-link.md](debug-link.md), `tools/rfoundry_debug.py`). It must work
without a computer.

## 1. What exists

| Piece | Where | Use here |
|---|---|---|
| Web Serial link to the radio | `site/static/js/flasher/link.js` (frames, CRC, `SerialStream`, `browserErrorKey`), `editor/app.js` (port chooser with the cable vendors 0403, 1A86, 10C4, 067B) | the transport, unchanged |
| The radio's SAT commands | `docs/spec/debuglink.md` 3.24: TLE, LOAD, SAVE, STORED, DELETE, TIME, LOOK, PASS, BENCH, CLEAR | everything the page needs; no firmware change |
| The update loop | `tools/rfoundry_debug.py` `sat_update`, `parse_tles`, `tle_checksum`, `tle_epoch`, `tle_catalog` | port to JavaScript, same rules |
| Diagnostics | debug-link view sections (`tasks`, `sat`, `crash`, `version`) | read-only panels |
| Test style | `site/tests/*.test.mjs` (Node's built-in test runner, no packages) | the same way for this page's logic |

The page copies the small parts of `link.js` it needs (about 200 lines: the frame, the CRC-16, the serial stream and
the error texts) instead of importing them, so it stands alone; the code is MIT licensed and keeps its header.

Nothing new is needed in the firmware. Every command is already there and already host-tested.

## 2. Decisions

| Question | Choice | Why |
|---|---|---|
| Where it lives | `tools/sat-web/`: `index.html`, a few ES modules, one stylesheet, a web app manifest and a small service worker. Static files, no build step, no framework | Standalone, so it can be hosted anywhere and nothing of the RadioFoundry site changes |
| Hosting | The owner's static host: GitHub Pages on the fork (free, https; the owner turns it on in the repository's settings), or Cloudflare Pages or Netlify pointed at the folder | Web Serial needs a secure context (https or localhost), so a phone needs a real https address |
| "Local first" | `python3 -m http.server` in the folder, opened at `http://localhost:8000` on the computer (localhost counts as secure). The phone can use the same with `adb reverse tcp:8000 tcp:8000`, or the hosted copy | A phone can't use Web Serial on `http://<computer's address>:8000` (not a secure context) |
| On the phone | Added to the home screen ("install"), which opens it like an app and, with the service worker, offline with the last download kept | The page has to work in a field with no coverage |
| Data leaving the page | Only the download of the element sets from the source. Nothing about the radio or position is sent anywhere | Privacy; no account, no server of ours |
| Element set source | CelesTrak's amateur group first (the host tool's default), with a pasted-text and a file option, and the last download kept in the browser | CelesTrak asks clients not to download the same group more often than every two hours; a kept copy is also what works without data coverage |
| Language | Plain JavaScript modules, English only at first; no framework, no build step | Smallest thing that works; translation can come later |

## 3. The page

Sections, top to bottom; a section that needs the radio says "Connect" until it has one.

| Section | What it does |
|---|---|
| Connect | One button, `navigator.serial.requestPort` with the cable filters, as the editor does. Shows the radio's `VERSION` (board, release, features list) and whether the build has `SAT` |
| Clock | Shows the radio's time and the phone's. One button sets the radio's clock from the phone (`SAT TIME`), which also gives a radio with a battery-backed clock its time. This is the answer to "no GPS and no computer" |
| Satellites on the radio | The stored list (`STORED` for each slot): name, catalog number, frequencies, the element set's age in days, in the warning color past a threshold. A Delete on each row (confirm first) |
| Update | The source (CelesTrak, pasted text, or a file), a table of the sets found marked "newer", "same" or "older than the radio's", the choice of which to store (default: those already stored and newer), and "Add" by catalog number for one not stored. Then the loop, one line of progress each, and a re-read of the list to show what the radio now holds |
| Diagnostics | Read-only panels: the `tasks` view (free heap, the least free stack and which task), the last crash record if any (`crash.*`), `sat` view (store state, slots), the model's time per run (`BENCH`). A "Copy report" button puts them all in the clipboard for pasting to a bug report |
| Not on this page | Anything that writes outside the satellite area: no clear-all without a typed confirmation, no firmware, no codeplug |

The update loop is `sat_update` as it is in `rfoundry_debug.py`: for each chosen set, `TLE` line 1, `TLE` line 2
(each in two 64-byte pieces), `LOAD`, check the reply's error bytes, then `SAVE` with the stored profile so a refresh
never changes what the satellite is set up to do. A set is used only if both lines are 69 characters with correct
check digits, the catalog numbers agree, and its epoch is later than the stored one (with the host tool's one-second
tolerance). The page must refuse in the same cases the tool does and say why in plain words.

## 4. Steps

| Step | Work | Done when |
|---|---|---|
| 0. Prove the phone path | No code. On the phone, with the cable on its OTG adapter, open the RadioFoundry editor page (only as a connectivity test of the phone, cable and radio) and press Connect | The port chooser lists the cable and the radio answers. **Do this first**: if the phone's browser or the cable's USB chip can't give a serial port, the rest changes (section 5) |
| 1. The logic, with tests | `tools/sat-web/tle.js`: parse, check digits, catalog, epoch, name, the "newer" rule. `link.js` (copied, trimmed) and `sat.js`: the SAT command encoders and decoders. Node tests in `tools/sat-web/test/` against the same vectors as `tools/rfoundry_debug.py`'s self-test and the firmware's `satLinkTest.c` | The tests pass, and a TLE the Python tool refuses is refused here with the same reason |
| 2. A fake radio | A small one in `tools/sat-web/test/`: the SAT commands and a store, behind the same stream interface the page uses | The page runs end to end on the computer with no radio, and in the Node tests |
| 3. The page: connect, clock, list | `index.html`, `app.js`, the stylesheet; laid out for a phone first | A radio's list shows on the computer; the clock button sets it |
| 4. Update | Source, table, loop, progress, errors | A real update of three satellites on the computer matches what `rfoundry_debug.py sat update` does (same epochs on the radio afterwards) |
| 5. Diagnostics | The panels and the copy button | The same numbers `rfoundry_debug.py view` prints |
| 6. The phone | Publish to the owner's host, open it on the phone, repeat 3 to 5 | An update from the phone, then Tools > Satellites on the radio shows the new ages |
| 7. Polish | Home-screen install and the service worker, the offline copy of the last download, "last updated" per satellite, a staleness warning, a short README in the folder and a pointer from `docs/user/24-satellites.md`, the Node tests in CI | CI green; the README says how to host it |

Each step is a commit on the branch; steps 1, 2 and the Node tests need no radio and no phone.

## 5. Risks, and what to do about each

| Risk | Detail | Handling |
|---|---|---|
| The phone's browser has no Web Serial, or not for this cable | Android Chrome gives serial ports only to some USB serial chips; the cable's chip (FTDI, CH340, CP210x, PL2303) decides. Not verified here | Step 0 answers it before any code. If it fails: another cable, or Chrome's own settings; the fallback is a page that downloads a file and a note to use the computer, which loses the point, so the step matters |
| The download is blocked by the browser | CelesTrak's CORS headers couldn't be checked from the build machine (the network there refused it) | Test from a browser in step 4. The pasted-text and file options always work; a small proxy would be the last resort and is not wanted (it would be a server to run) |
| The port is busy | One program holds a serial port at a time: the editor tab, the flasher tab or a terminal | The page says so (`browserErrorKey` already maps the errors) and closes its own port when the tab hides |
| A phone sleeping mid-update | Android may suspend a hidden tab during the loop, and a write to the radio's flash is about 50 ms per satellite | Keep the screen awake (`navigator.wakeLock`) during the loop; every satellite is stored whole or not at all (a record carries a CRC and a generation), so an interrupted update leaves the list valid, with some satellites not yet refreshed |
| A wrong clock on the phone | The set time button would put the error on the radio | Show both times and the difference before the button; refuse a phone clock before 2026 (the radio's clock does) |
| Stale or bad element sets | An old file pasted in | The "older than the radio's" and "over 30 days" marks; the epoch rule keeps the newer set |
| Two copies of the logic | JavaScript and Python each carry the update rules | One shared vector file for both tests (a JSON of TLEs and the expected outcome), so they can't drift |

## 6. Later

- Pass alarms and "load the passes for tonight" need the radio to hold them; not part of this tool.
- Editing a satellite's frequencies, tones and arming settings on the page (the SAVE command already takes them): a
  small form per row, after the update works.
- A companion that feeds the phone's GPS position and time to the radio over the same cable (the host link takes
  NMEA, [satellite.md](../spec/satellite.md) 4.1), which would give the radio a fix indoors.
