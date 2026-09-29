#!/bin/sh
# Regenerates the GKD 350H Ultra sysroot from first sources - the GKD's
# counterpart of tools/fetch-brick-sysroot.sh, on the same terms (ADR-0012):
# the libraries are ROCKNIX's binaries and the headers are upstream SDL's, so
# the repository records how to obtain them rather than a copy of them.
#
#   Libraries: pulled over SSH from the device's own /usr/lib, plus the
#              dependency closure needed to satisfy the linker.
#   Headers:   upstream libsdl-org/SDL release 2.32.6, version-matched to the
#              library vendor ROCKNIX ships (SDL-release-2.32.6-0-g6510d6ccb,
#              verified 2026-09-29). Pinned by sha256.
#
# Needs: the GKD reachable as root over SSH, curl, and the
# diatom-brick-toolchain image (docker), whose readelf resolves the closure.
# The Brick's toolchain builds for the GKD too: its glibc 2.31 is the floor the
# binary demands, and the device's 2.40 satisfies it.
#
#   tools/fetch-gkd-sysroot.sh [sysroot-dir]      default: sysroot/gkd
#   GKD_HOST=root@<ip> overrides the device address.
set -eu

SDL_VERSION=2.32.6
# Recorded 2026-09-29 from the libsdl-org/SDL release asset, trust on first
# use. Guards against a changed tarball, not against a malicious first fetch.
SDL_SHA256=6a7a40d6c2e00016791815e1a9f4042809210bdf10cc78d2c75b45c4f52f93ad
SDL_URL="https://github.com/libsdl-org/SDL/releases/download/release-${SDL_VERSION}/SDL2-${SDL_VERSION}.tar.gz"
IMAGE=diatom-brick-toolchain
GKD_HOST=${GKD_HOST:-root@192.168.0.55}

SYSROOT=${1:-sysroot/gkd}
LIBDIR="$SYSROOT/usr/lib"
INCDIR="$SYSROOT/usr/include/SDL2"
CACHE="$SYSROOT/.cache"

say()  { printf 'sysroot: %s\n' "$*"; }
fail() { printf 'sysroot: ERROR: %s\n' "$*" >&2; exit 1; }
dev()  { ssh -o ConnectTimeout=5 "$GKD_HOST" "$@"; }

dev true 2>/dev/null || fail "no device over ssh at $GKD_HOST"
docker image inspect "$IMAGE" >/dev/null 2>&1 \
    || fail "toolchain image missing; build it first:
  docker build -f tools/brick-toolchain.Dockerfile -t $IMAGE tools"

mkdir -p "$LIBDIR" "$INCDIR" "$CACHE"

# --- libraries, from the device -------------------------------------------

# scp follows symlinks, so copying the SONAME yields a real file under the
# SONAME's own name, which is exactly what the linker wants to find.
pull_lib() {
    scp -q "$GKD_HOST:$1" "$LIBDIR/" 2>/dev/null
}

say "pulling libSDL2 from device /usr/lib"
pull_lib /usr/lib/libSDL2-2.0.so.0 || fail "could not pull libSDL2"
ln -sf libSDL2-2.0.so.0 "$LIBDIR/libSDL2.so"   # -lSDL2 resolves through this

# Dependency closure; glibc members come from the cross toolchain at link time.
GLIBC_SONAMES='libc.so.6 libm.so.6 libdl.so.2 libpthread.so.0 librt.so.1
libresolv.so.2 libutil.so.1 ld-linux-aarch64.so.1 libgcc_s.so.1'

needed() {
    docker run --rm -v "$(cd "$LIBDIR" && pwd)":/libs "$IMAGE" sh -c \
      'for f in /libs/*.so*; do aarch64-linux-gnu-readelf -d "$f" 2>/dev/null; done' \
      | sed -n 's/.*(NEEDED).*\[\(.*\)\].*/\1/p' | sort -u
}

pass=0
while [ "$pass" -lt 10 ]; do
    pass=$((pass + 1))
    missing=""
    for so in $(needed); do
        [ -e "$LIBDIR/$so" ] && continue
        skip=0
        for g in $GLIBC_SONAMES; do [ "$so" = "$g" ] && skip=1; done
        [ "$skip" = 1 ] || missing="$missing $so"
    done
    [ -z "$missing" ] && break
    for so in $missing; do
        # /lib is a symlink to /usr/lib on ROCKNIX; one place to look.
        dev "test -e /usr/lib/$so" 2>/dev/null || fail "$so needed but not found on device"
        say "pulling dependency /usr/lib/$so"
        pull_lib "/usr/lib/$so" || fail "could not pull /usr/lib/$so"
    done
done
[ "$pass" -lt 10 ] || fail "dependency closure did not converge"

# --- headers, from upstream ------------------------------------------------

TARBALL="$CACHE/SDL2-${SDL_VERSION}.tar.gz"
if [ ! -e "$TARBALL" ] || ! echo "$SDL_SHA256  $TARBALL" | sha256sum -c - >/dev/null 2>&1; then
    say "downloading SDL2 $SDL_VERSION headers from upstream"
    curl -sSLf -o "$TARBALL" "$SDL_URL"
fi
echo "$SDL_SHA256  $TARBALL" | sha256sum -c - >/dev/null \
    || fail "SDL2 tarball hash mismatch"

# Only include/ - see fetch-brick-sysroot.sh on SDL_config_minimal.h.
tar xzf "$TARBALL" -C "$CACHE" "SDL2-${SDL_VERSION}/include"
cp "$CACHE/SDL2-${SDL_VERSION}/include/"*.h "$INCDIR/"

# --- provenance ------------------------------------------------------------

{
    echo "generated: $(date +%Y-%m-%d)"
    echo "device:    $GKD_HOST ($(dev hostname))"
    echo "kernel:    $(dev uname -r)"
    echo "glibc:     $(dev '/usr/lib/libc.so.6' | head -1)"
    echo "sdl2:      $(dev "strings /usr/lib/libSDL2-2.0.so.0 | grep -m1 '^SDL-release'")"
    echo "headers:   SDL2 ${SDL_VERSION} upstream, sha256 ${SDL_SHA256}"
} > "$SYSROOT/PROVENANCE"

say "done:"
ls -la "$LIBDIR"
cat "$SYSROOT/PROVENANCE"
