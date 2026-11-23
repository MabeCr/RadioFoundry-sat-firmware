# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
# DM-1701 feature defaults (config/features.mk lists them all). 720 KB for code and read-only data: flash sectors 3-9;
# the codec sectors 10 and 11 are never linked into (boards/dm1701/memory.ld, docs/spec/codec-placement.md).
# 160x120 display. The fault trace, the large font and LTO follow the platform (platform/stm32f4/config.mk)
# Safe area: the lens hides about 5 px left and right (bench, 2026-10-09). Top and bottom are 0: the 5 px there were
# dead space on the radio (owner, 2026-10-11). Left and right stay 5 until checked on the radio the same way
CONFIG_DISPLAY_INSET ?= 5
CONFIG_DISPLAY_INSET_TOP ?= 0
CONFIG_DISPLAY_INSET_BOTTOM ?= 0
# APRS receive takes the HR_C6000's FM receive audio from the I2S stream (docs/spec/aprs.md 5.3). The iGate (RF to
# APRS-IS through tools/aprs_gate.py, 1.9 KB) is off by default
CONFIG_APRS_IGATE ?= n
MAXIMAL_VARS := LTO=1
# Left out of tools/ci.sh's maximal configuration: PPP stands in for the USB network adapter on UART-only radios
# (docs/spec/ppp.md), and this one has the adapter; the APRS iGate is off by default (above); French, off by
# default, and Italian (the other boards build it); the satellite tracking of core/sat (and its sound, chart, alert and splash) is left out too: with P25 and NXDN
# beside it the image overflows the flash and the RAM, which is why SATELLITE_VARS below builds it without them; ci builds
# that as its own "satellite" configuration
MAXIMAL_SKIP := NET_PPP APRS_IGATE LANG_FR LANG_IT LANG_DEFAULT_IT SAT SAT_SOUND SAT_CHART SAT_ALERT SAT_SPLASH
# The satellite build to flash (tools/satellite.sh, the cloud image): the radio's own defaults plus satellite tracking and
# element sets by sound, without P25 and NXDN (about 6.6 KB of RAM and 43 KB of flash each, which this radio's RAM can't
# spare beside them)
SATELLITE_VARS := CONFIG_SAT=y CONFIG_SAT_SOUND=y CONFIG_P25=n CONFIG_NXDN=n
# What the radio's hardware can't do, one switch per line with the reason (tools/boards.py checks there is one;
# docs/build-config.md, "Unsupported, not written, unconfirmed"): ci's maximal configuration leaves these off
BOARD_UNSUPPORTED += FONT_XL	# the 160x120 panel is too small for the large font (it is for 240 px and wider)
BOARD_UNSUPPORTED += FONT_BIG	# the 9x18 font is drawn only on panels 220 px and wider; this one is 160x120
CONFIG_AM_RX ?= n
BOARD_UNSUPPORTED += AM_RX	# no air band receiver: the AT1846S is FM only (docs/spec/airband-and-broadcast.md)
CONFIG_BCAST_FM ?= n
BOARD_UNSUPPORTED += BCAST_FM	# no broadcast receiver (docs/spec/airband-and-broadcast.md)
BOARD_UNSUPPORTED += NAND_STORAGE	# the data flash is a 16 MB SPI NOR (docs/spec/hardware-dm1701.md, the overview)
BOARD_UNSUPPORTED += VIBRATE	# no vibration motor (docs/user/23-radios.md, What the DM-1701 lacks)
BOARD_UNSUPPORTED += FAULT_TRACE	# needs a host UART: the host port is USB, and the one UART is the GPS line
# GPS stays on: no module from the factory, but owners fit one on PA10 / PA9 (hardware-dm1701.md 14.2). Detected at run
# time in the background: a radio without one stops listening after 5 s of silence

# A Chinese character is 16 px tall (core/ui/cjk_font.h) and these layouts draw their words in the 6x12 font:
# there is no room for a 16 px line, and the CJK face must not be built in (docs/build-config.md)
BOARD_UNSUPPORTED += LANG_ZH	# a Chinese line needs 16 px; this layout draws its words in a smaller font
BOARD_UNSUPPORTED += LANG_DEFAULT_ZH	# needs LANG_ZH

BOARD_NOT_WRITTEN += SCAN_LIST	# no scan list area in this radio's flash map yet (docs/spec/scan-list.md)
