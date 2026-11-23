<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 The RadioFoundry Authors -->

# This fork

A fork of [radiofoundry/radiofoundry](https://github.com/radiofoundry/radiofoundry) with satellite work on top. It builds
the same radios from the same tree: what differs is listed below, and each item has its spec and user page.

## What it adds

| Change | Switch | Where |
|---|---|---|
| Satellite tracking: list, passes, Tracking with Doppler, signal chart, pass alerts, sky plot splash, delete | `CONFIG_SAT`, `SAT_CHART`, `SAT_ALERT`, `SAT_SPLASH` | [spec](docs/spec/satellite.md), [user](docs/user/24-satellites.md) |
| Element sets by sound (a phone plays them to the microphone) | `CONFIG_SAT_SOUND` | [spec](docs/spec/sound-transfer.md) |
| Scan lists (channels from any zone) and the scan dwell setting | `CONFIG_SCAN_LIST` | [spec](docs/spec/scan-list.md), [user](docs/user/12-scan.md) |
| Flip screen (the picture turned 180 degrees) | none: an optional `hal_display_t.set_flip` | [spec](docs/spec/ui.md) 2.8 |
| APRS path ARISS; ISS VHF and UHF as APRS frequencies | none | [user](docs/user/13-aprs.md) |
| Upstream's P25 lockout (pull request 745) | `CONFIG_P25_TRUNK` | [spec](docs/spec/p25-trunking.md) |

`CONFIG_SAT` and upstream's `CONFIG_SATELLITE_TLE` are alternatives: a build has one. The debug link command for ours is
`0x30` (upstream took `0x20`).

## Images

`.github/workflows/satellite-image.yml` builds a codec-less image for each board that lists `SATELLITE_VARS` on a push to
`main` or a `claude/**` branch, and publishes it as the pre-release `satellite-<board>-<commit>`; the newest five of
each board are kept. Nothing in an image has been tried on a radio unless its bring-up record says so.

## Keeping up with upstream

```bash
git fetch upstream
git merge upstream/main
```

Where it conflicts: `core/radio/radio_sm.c`, `core/ui/ui_menus.c`, `core/ui/ui_svc.h`, the contact sheets
(`docs/ux/screens/sheets/*.png`: take either side, then `make -C tests/host ui-gallery`) and the translation catalogs
(`python3 tools/i18n.py --rewrite`, then `--sync`). Afterwards: `make -C tests/host`, `JOBS=8 tools/ci.sh`, and the
pull request's CI for the C-SKY boards, which need `csky-elf-gcc`.

## Known open items

- The Chinese catalog fails the translation check on characters with no glyph in `core/ui/cjk_font_data.c`; regenerate
  it with `tools/cjkfont.py --po` and Noto Sans CJK.
- `core/radio/radio_sm.c` is past 10,000 lines. gcovr 7.0 can't read such line numbers, so `tools/coverage.sh` ignores
  parse errors; the real fix is to split the file.
- Not yet tried on a radio: pass alerts, the P25 lockout over the air, the P25 channel setups.
