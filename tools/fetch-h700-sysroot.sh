#!/bin/sh
# The H700 sysroot: TortOS's SDL2 for BaseOS (its mk/fetch-h700-sysroot.sh),
# built into sysroot/h700. One recipe and one patch for both repositories, so
# the diatom that runs links the very SDL2 the card ships.
#
#   tools/fetch-h700-sysroot.sh [TortOS checkout]   default: ../TortOS
set -eu
ROOT=$(cd "$(dirname "$0")/.." && pwd)
TORTOS=${1:-$ROOT/../TortOS}
[ -x "$TORTOS/mk/fetch-h700-sysroot.sh" ] ||
	{ echo "no $TORTOS/mk/fetch-h700-sysroot.sh (pass the TortOS checkout)" >&2; exit 1; }
sh "$TORTOS/mk/fetch-h700-sysroot.sh" "$ROOT/sysroot/h700"
