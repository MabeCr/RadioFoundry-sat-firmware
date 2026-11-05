<!-- SPDX-License-Identifier: MIT -->

# Satellite TLE tool

A web page that updates a radio's satellite element sets (TLEs), sets its clock from the phone or computer and reads its
diagnostics, over the programming cable. It runs in the browser (Web Serial: Chrome or Edge, on a computer, or Chrome on
an Android phone with the cable on an OTG adapter). It needs a radio running a RadioFoundry build with satellite tracking
(`SAT` in the features list of its `version`). It is a standalone page: plain files, no build step, nothing of the
RadioFoundry web site. The plan is [docs/dev/sat-tle-web-tool-plan.md](../../docs/dev/sat-tle-web-tool-plan.md).

## Use it

1. Open the page (below), plug the cable into the radio and the device, switch the radio on.
2. **Connect radio**, and choose the cable.
3. **Clock**: *Set the radio's clock from this device* (the radio refuses a time before 2026).
4. **Update element sets**: Download (CelesTrak's amateur group, kept in the browser and not repeated within two hours),
   Paste, or File. The page lists the satellites on the radio with whether the data has a newer set; add one that isn't
   stored by catalog number (or an exact name); **Update the radio**.
5. **Diagnostics**: read-only: version, tasks (free heap and stack), the last crash record, the satellite store and the
   orbit model's speed. *Copy report* puts it on the clipboard for a bug report.

Only one program may hold the serial port at a time: close any other tab or tool using the cable. The page keeps the
screen awake while it updates.

## Run it on a computer

```bash
cd tools/sat-web
python3 -m http.server 8000
```

Open `http://localhost:8000` in Chrome or Edge. (`localhost` counts as a secure context, which Web Serial needs.)

## Put it on a phone

Web Serial needs `https`, so the phone needs the page on a real address. Any static host works; the page is the files in
this folder (`index.html`, `app.js`, `style.css`, `sw.js`, `manifest.webmanifest`, the icons and `lib/`; `test/` is not
needed).

- **GitHub Pages**: in the repository's Settings, Pages, publish from a branch and folder that holds these files (the
  Pages "folder" choices are the root or `/docs`: copy this folder there, or use a GitHub Action that publishes it).
- **Cloudflare Pages or Netlify**: point the project at this folder, no build command.

Open the address in Chrome on the phone and, once it works, *Add to Home screen* / *Install app*: it then opens like an
app and, with the service worker (https only), works with no coverage using the last element set download. To try the
local copy on the phone before publishing: `adb reverse tcp:8000 tcp:8000`, then open `http://localhost:8000` on the phone.

## Test

```bash
node --test tools/sat-web/test/*.test.mjs          # the logic against a fake radio, and the host tool's outcomes
python3 tools/sat-web/test/make_vectors.py --check # the shared vectors still match tools/rfoundry_debug.py
node tools/sat-web/test/page.e2e.mjs [shot.png]    # the page in Chromium (Playwright) at a phone's size
```

`test/vectors.json` is made by the Python tool (`make_vectors.py`): the JavaScript must agree with it, so the two can't
drift. Change the Python rules, run `make_vectors.py`, then fix the JavaScript until the tests pass.

## Files

| File | What |
|---|---|
| `lib/tle.js` | check digits, catalog numbers (Alpha-5), epochs, parsing, "newer or not" |
| `lib/frame.js`, `lib/link.js` | the debug link: frames, requests, version, view sections |
| `lib/sat.js` | the SAT command and the update loop (the same rules as `rfoundry_debug.py sat update`) |
| `lib/serial.js` | Web Serial as a byte stream |
| `test/fakeradio.js` | a radio's debug link in memory |
