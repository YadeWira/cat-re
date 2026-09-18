#!/usr/bin/env bash
# Build the static Linux dependency (OpenJPEG) that `catre` / `catre-static`
# link against, into a PERSISTENT directory.
#
# Why: the deps used to live in /tmp, which is a tmpfs here — after a reboot
# `make catre` failed with "openjpeg.h: No such file or directory".
#
# Output layout (consumed by the Makefile):
#   $OUT/include/openjpeg-2.5/openjpeg.h
#   $OUT/lib/libopenjp2.a
#
# Requirements (Debian/Ubuntu): zlib1g-dev cmake curl gcc
#
# Usage: scripts/build-linux-deps.sh [OUTDIR]   (default: ~/.cache/catre-deps)
set -euo pipefail

OUT="${1:-$HOME/.cache/catre-deps}"
OPJ_VER=2.5.3
mkdir -p "$OUT"; cd "$OUT"

if [ -f "$OUT/lib/libopenjp2.a" ] && [ -f "$OUT/include/openjpeg-2.5/openjpeg.h" ]; then
  echo ">> deps already built in $OUT"; exit 0
fi

echo ">> fetching OpenJPEG $OPJ_VER"
[ -f opj.tar.gz ] || curl -sL -o opj.tar.gz \
  "https://github.com/uclouvain/openjpeg/archive/refs/tags/v${OPJ_VER}.tar.gz"
[ -d "openjpeg-${OPJ_VER}" ] || tar xzf opj.tar.gz

echo ">> building static libopenjp2"
rm -rf bld-linux; mkdir bld-linux
( cd bld-linux
  cmake "../openjpeg-${OPJ_VER}" \
    -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$OUT" \
    -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
    -DBUILD_SHARED_LIBS=OFF -DBUILD_CODEC=OFF \
    -DBUILD_DOC=OFF -DBUILD_TESTING=OFF >/dev/null
  make -j"$(nproc)" >/dev/null
  make install >/dev/null )

echo ">> done: $(ls "$OUT"/lib/libopenjp2.a)"
