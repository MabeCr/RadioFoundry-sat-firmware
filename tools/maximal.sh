#!/bin/sh
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
# A board's maximal configuration: every switch on except those it can't do (BOARD_UNSUPPORTED, BOARD_NOT_WRITTEN), its
# MAXIMAL_SKIP and the bench diagnostics, then the board's MAXIMAL_VARS (boards/<b>/config.mk). It is what tools/ci.sh
# builds and stack-checks as "maximal"; a radio with little RAM cuts what it must in MAXIMAL_VARS so this fits.
#   tools/maximal.sh BOARD [make arguments...]   build it:   tools/maximal.sh dm32 stack-check
#                                                BUILD defaults to build/<board>-maximal
#   tools/maximal.sh --args BOARD                print the make variables only (tools/ci.sh uses this)
# In the C-SKY container: tools/docker/csky-toolchain/run.sh tools/maximal.sh dm32
set -eu
cd "$(dirname "$0")/.."
# Switches the maximal configuration leaves off on every board: bench diagnostics, never in a release
MAXIMAL_SKIP="AUDIO_PROBE"

args() {
	board=$1
	unsupported=" $(make -s BOARD="$board" print-unsupported) $(make -s BOARD="$board" print-not-written) $(make -s BOARD="$board" print-maximal-skip) $MAXIMAL_SKIP "
	for kv in $(make -s BOARD="$board" print-config); do
		f=${kv%%=*}
		case "$unsupported" in *" $f "*) ;; *) printf 'CONFIG_%s=y ' "$f" ;; esac
	done
	make -s BOARD="$board" print-maximal-vars
}

if [ "${1:-}" = "--args" ]; then
	[ $# -eq 2 ] || { echo "usage: tools/maximal.sh --args BOARD" >&2; exit 2; }
	args "$2"
	exit 0
fi
[ $# -ge 1 ] || { echo "usage: tools/maximal.sh BOARD [make arguments...]" >&2; exit 2; }
board=$1
shift
# shellcheck disable=SC2046
exec make BOARD="$board" BUILD="${BUILD:-build/$board-maximal}" $(args "$board") "$@"
