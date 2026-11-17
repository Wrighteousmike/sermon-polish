#!/bin/sh
# Builds LAME as a static library into native/third_party/lame-install (or lame-install-<arch> when
# cross-building). Run scripts/fetch_deps.sh first.
#   ARCH=x86_64 ./scripts/build_lame.sh     # Intel build on an Apple silicon Mac (macOS only)
set -e
T="$(cd "$(dirname "$0")/../third_party" && pwd)"
export MACOSX_DEPLOYMENT_TARGET=11.0
SUFFIX=""; HOST_ARGS=""; CFL="-O2"
if [ -n "$ARCH" ] && [ "$ARCH" != "$(uname -m)" ]; then
  SUFFIX="-$ARCH"
  CFL="-O2 -arch $ARCH"
  HOST_ARGS="--host=$ARCH-apple-darwin"
fi
P="$T/lame-install$SUFFIX"
TMPP="${TMPDIR:-/tmp}/sermon-lame-prefix$SUFFIX"   # libtool breaks on an -rpath containing spaces
# Each architecture builds in its own copy of the source (configure refuses a source path with spaces
# for out-of-tree builds, and the native tree is already configured).
B="$T/lame-3.100"
if [ -n "$SUFFIX" ]; then
  B="$T/lame-src$SUFFIX"
  rm -rf "$B"; cp -R "$T/lame-3.100" "$B"; (cd "$B" && make distclean >/dev/null 2>&1 || true)
fi
# libtool breaks on a build directory containing spaces (the x86 vector routines hit it), so build through a
# space-free symlink to the same directory.
LINK="${TMPDIR:-/tmp}/sermon-lame-link$SUFFIX"; rm -f "$LINK"; ln -s "$B" "$LINK"
cd "$LINK"; make distclean >/dev/null 2>&1 || true
CC="${CC:-cc}" CFLAGS="$CFL" LDFLAGS="${CFL#-O2}" ./configure $HOST_ARGS --prefix="$TMPP" --disable-shared --enable-static \
  --disable-frontend --disable-decoder --disable-gtktest --disable-dependency-tracking
make -j"$(sysctl -n hw.ncpu 2>/dev/null || nproc 2>/dev/null || echo 4)"
make install || true  # libtool install can fail on macOS; the copy below covers it
mkdir -p "$P/lib" "$P/include/lame"
cp libmp3lame/.libs/libmp3lame.a "$P/lib/"
cp include/lame.h "$P/include/lame/"
