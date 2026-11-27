<!-- SPDX-License-Identifier: MIT -->

# Theme packs

Theme packs are data installed in a board-provided data-flash area. They contain palette values and names only. They do
not contain executable code, settings, radio configuration, or firmware images. The user interface continues to use
the built-in themes when no valid pack is installed.

## Requirements

- Installing, replacing, or removing a pack changes only the mapped theme-pack area.
- The firmware validates the whole pack before making any entry available.
- An interrupted or invalid write makes the pack unavailable and falls back to built-in themes.
- Pack entries use theme IDs after the built-in IDs. When a selected pack entry is unavailable, the display falls back
  to the built-in Dark palette; the stored theme ID is kept so reinstalling the pack restores the selection.
- A pack update is compatible with the theme-pack format version, not with arbitrary firmware internals. The installer
  checks the target radio and format before writing.
- The transfer interface exposes theme-pack operations only. It must not accept an arbitrary address or arbitrary flash
  write command.

## Pack format

All integer fields are little-endian. Version 1 uses this 20-byte header, followed by fixed-size entries:

| Header offset | Size | Field |
|---:|---:|---|
| 0 | 4 | ASCII magic `RFTP` |
| 4 | 2 | Format version, 1 |
| 6 | 2 | Header size, 20 |
| 8 | 4 | Payload size in bytes |
| 12 | 2 | Entry count, 1..21 |
| 14 | 2 | Reserved, zero |
| 16 | 4 | CRC-32 (IEEE 802.3) of the payload |

Each 60-byte entry contains a one-byte name length, a 23-byte UTF-8 name field padded with zeroes, then 18 RGB565
colors as unsigned 16-bit values. Color order is `bg`, `fg`, `dim`, `line`, `tx`, `rx`, `warn`, `idle`, `pill_fg`,
`pill_dark_fg`, `focus`, `hint_bg`, `priv`, `idle_fg`, `focus_fg`, `chnum`, `strip_bg`, `strip_warn`. Pack theme IDs
are 11 through 31. Built-in IDs 0 through 7 keep their meaning; IDs 8 through 10 are reserved so existing pack IDs
and stored selections do not move. A pack holds at most 21 themes and is limited to one
mapped erase unit. Unknown format versions, invalid names, a bad CRC, a mismatched payload length, or a length outside
the mapped area reject the pack.

The host pack builder takes a readable JSON source and emits a binary pack. For example:

```sh
python3 tools/theme_pack.py build themes/sample-theme-pack.json /tmp/my-themes.rftheme
python3 tools/theme_pack.py inspect /tmp/my-themes.rftheme
```

The host validates the complete binary before starting transfer. The radio erases only the mapped theme-pack sector at
transfer start, then accepts sequential bounded chunks and reads the completed pack back during commit. An interrupted
or invalid transfer makes the pack unavailable; the built-in themes remain available. Because this map provides one
erase unit, replacing a pack cannot preserve the previous pack across a power loss during the erase/program operation.

## Storage and board support

The theme-pack address and size are explicit members of `hal_flash_map_t`. A zero size means unsupported. The map's end,
board registry, write-protection checks, and overlap checks include the area. A board exposes it only where its memory
map documents an unused erase unit and `CONFIG_THEME_PACKS` is enabled. The generic code does not infer free space or
name a radio.

The first supported target's documented area is described in its hardware reference. Other boards/configurations keep
the map entry empty unless their own memory map documents an unused erase unit.

## Settings

The selected theme remains a small numeric setting. IDs 0 through 7 are built-in; IDs 8 through 10 are reserved; pack
themes use IDs 11 through 31. Pack removal does not rewrite codeplug, radio, channel, or other settings data.

## Installer

The website Tools page adds a dedicated theme-pack action after the radio reports support and its format version. The
firmware accepts a bounded sequence of begin, data, commit, info, and remove operations. It refuses writes while
transmitting and serializes flash access with the storage lock. Firmware upload and theme-pack upload are separate
operations with separate validation.
