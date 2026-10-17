#!/bin/sh
# The RG Rotate sysroot: Debian trixie's own arm64 packages, as the card's
# Armbian installs them - TortOS's mk/fetch-rotate-sysroot.sh, run into
# sysroot/rotate. One recipe for both repositories.
#
#   tools/fetch-rotate-sysroot.sh [TortOS checkout]   default: ../TortOS
set -eu
ROOT=$(cd "$(dirname "$0")/.." && pwd)
TORTOS=${1:-$ROOT/../TortOS}
[ -x "$TORTOS/mk/fetch-rotate-sysroot.sh" ] ||
	{ echo "no $TORTOS/mk/fetch-rotate-sysroot.sh (pass the TortOS checkout)" >&2; exit 1; }
sh "$TORTOS/mk/fetch-rotate-sysroot.sh" "$ROOT/sysroot/rotate"
