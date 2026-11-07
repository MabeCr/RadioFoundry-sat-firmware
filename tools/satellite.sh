#!/bin/sh
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
# A board's satellite build, the image to flash: the board's own defaults plus satellite tracking (CONFIG_SAT=y), then
# the board's SATELLITE_VARS (boards/<b>/config.mk: what a radio with little RAM cuts to make room). Not the maximal
# configuration (tools/maximal.sh), which turns every switch on, other languages included.
#   tools/satellite.sh BOARD [make arguments...]   build it:   tools/satellite.sh <board> stack-check
#                                                  BUILD defaults to build/<board>-satellite
# In the C-SKY container: tools/docker/csky-toolchain/run.sh tools/satellite.sh <board>
set -eu
cd "$(dirname "$0")/.."
[ $# -ge 1 ] || { echo "usage: tools/satellite.sh BOARD [make arguments...]" >&2; exit 2; }
board=$1
shift
vars=$(make -s BOARD="$board" print-satellite-vars)
case " $vars " in *" CONFIG_SAT=y "*) ;; *) vars="CONFIG_SAT=y $vars" ;; esac	# a board without a list: its defaults and SAT
# shellcheck disable=SC2086
exec make BOARD="$board" BUILD="${BUILD:-build/$board-satellite}" $vars "$@"
