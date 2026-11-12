# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
# DM-32UV feature defaults (config/features.mk lists them all). 880 KB of code space before the codec slot, so RAM is
# the limit: 211,456 B below the codec's RAM window (boards/dm32/memory.ld, docs/spec/hardware-dm32.md 17.1), the
# RTOS heap's 33 KB among them (freertos_board.h). Every feature the radio has hardware and glue for is on, and the
# image keeps about 0.5 KB of it free: a feature that needs more RAM cuts some first, or shrinks the heap if
# tasks.heap_min_free shows room (tests/hil/test_task_stacks.py)
# The host link runs on the programming cable's UART0; USB, PPP and TCP_CONNS follow the platform
# (platform/hrc7000/config.mk)
# The 240x320 panel at full resolution in 16 colors (display_dm32.c, board.mk). The 16x32 font is the large class's
# channel name; text is anti-aliased, and lists and labels are one size up (9x18)
CONFIG_FONT_XL ?= y
CONFIG_FONT_AA ?= y
# Follows FONT_AA (config/features.mk: BIG needs AA), so the nightly matrix's "FONT_AA off alone" build is a valid one
CONFIG_FONT_BIG ?= $(CONFIG_FONT_AA)
# The bezel hides the panel's edges, in panel pixels: text 6 px from the left or right edge was still clipped on the
# radio, so 10 px left and right (the 8 px measured on the right leaves no more 8 px columns), 5 px top and bottom
CONFIG_DISPLAY_INSET_LEFT ?= 10
CONFIG_DISPLAY_INSET_RIGHT ?= 10
CONFIG_DISPLAY_INSET_TOP ?= 5
CONFIG_DISPLAY_INSET_BOTTOM ?= 5
# VOX reads the microphone level through hal_power_t.mic_level, which this board doesn't have yet
CONFIG_VOX ?= n
# The platform's three TCP connections; a fourth costs about 1.4 KB of main RAM. ci's maximal configuration has
# every switch but USB and USB_NETWORK (platform/hrc7000/config.mk), and NAND_STORAGE (below), and also keeps about
# 0.5 KB free
BOARD_UNSUPPORTED += NAND_STORAGE	# the data flash is SPI NOR, GD25Q128 class (docs/spec/hardware-dm32.md 10)
# What the hardware can do but this radio's driver or glue doesn't yet (docs/build-config.md, "Unsupported, not
# written, unconfirmed"): off by default, and off in ci's maximal configuration too
BOARD_NOT_WRITTEN += VOX	# no hal_power_t.mic_level hook yet
BOARD_NOT_WRITTEN += AUDIO_PROBE	# no board.audio_tap yet

# Satellite tracking costs this radio about 2.8 KB of RAM and 2 KB more RTOS heap (freertos_board.h), and the default image
# has 16 B free, so a build with it cuts what costs RAM. With these off the satellite build links with about 23.7 KB free
# (measured 2026-11-05, docs/dm32-satellite-plan.md), keeps the dual watch and APRS receive, and passes the stack check. A
# feature that needs RAM later cuts from this list or shrinks the heap
SATELLITE_CUTS := CONFIG_SFR=n CONFIG_SFR_CWID=n CONFIG_SFR_DUALPATH_RX=n CONFIG_DMR_PRIVACY=n CONFIG_DMR_TMS=n \
	CONFIG_DMR_ARS=n CONFIG_DMR_LRRP=n CONFIG_DMR_CONTROL_STATION=n CONFIG_SNMP=n CONFIG_HOTSPOT=n \
	CONFIG_HOTSPOT_SPLIT=n CONFIG_LANG_ES=n CONFIG_LANG_DEFAULT_ES=n CONFIG_NET_USRP=n CONFIG_ZONE_SHARE=n \
	CONFIG_NET_HTTP=n CONFIG_NET_HTTP_GZIP=n CONFIG_NET_WEBCTL=n CONFIG_DMR_ROAMING=n
# ci's maximal configuration (tools/maximal.sh): every switch on, so every code path is built and linked, with those cuts.
# It is a coverage build, never an image to flash: it also turns the Italian language on, as the default
MAXIMAL_VARS := $(SATELLITE_CUTS)
# The satellite build to flash (tools/satellite.sh, the cloud image): the radio's own defaults (English only, no vibration
# alerts) plus satellite tracking and the cuts
SATELLITE_VARS := CONFIG_SAT=y CONFIG_SAT_SOUND=y $(SATELLITE_CUTS)
