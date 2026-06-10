#!/usr/bin/env bash
# Build the static Windows dependencies (zlib + OpenJPEG) that `catre.exe`
# links against, for both 64-bit and 32-bit mingw-w64 targets.
#
# Output layout (consumed by the Makefile's catre-win64 / catre-win32 targets):
#   $OUT/out-x64/{lib/libz.a,lib/libopenjp2.a,include/...}
#   $OUT/out-x86/{lib/libz.a,lib/libopenjp2.a,include/...}
#
# Requirements (Debian/Ubuntu):
#   sudo apt install gcc-mingw-w64-x86-64 gcc-mingw-w64-i686 cmake curl
#
# Usage: scripts/build-win-deps.sh [OUTDIR]   (default: /tmp/winbuild)
set -euo pipefail

OUT="${1:-/tmp/winbuild}"
ZLIB_VER=1.3.1
OPJ_VER=2.5.3
mkdir -p "$OUT"; cd "$OUT"

echo ">> fetching sources"
[ -f zlib.tar.gz ] || curl -sL -o zlib.tar.gz \
  "https://github.com/madler/zlib/releases/download/v${ZLIB_VER}/zlib-${ZLIB_VER}.tar.gz"
[ -f opj.tar.gz ] || curl -sL -o opj.tar.gz \
  "https://github.com/uclouvain/openjpeg/archive/refs/tags/v${OPJ_VER}.tar.gz"
[ -d "zlib-${ZLIB_VER}" ]     || tar xzf zlib.tar.gz
[ -d "openjpeg-${OPJ_VER}" ]  || tar xzf opj.tar.gz

build_one() {
  local host=$1 prefix=$2 sysproc=$3
  echo ">> [$host] zlib"
  ( cd "zlib-${ZLIB_VER}"
    make distclean >/dev/null 2>&1 || true
    CC=${host}-gcc AR=${host}-ar RANLIB=${host}-ranlib make -f win32/Makefile.gcc \
      PREFIX=${host}- libz.a >/dev/null 2>&1 \
      || { ./configure --static >/dev/null 2>&1
           CC=${host}-gcc AR=${host}-ar RANLIB=${host}-ranlib make libz.a >/dev/null 2>&1; }
    mkdir -p "$prefix/lib" "$prefix/include"
    cp libz.a "$prefix/lib/"; cp zlib.h zconf.h "$prefix/include/" )

  echo ">> [$host] openjpeg"
  cat > "$OUT/tc-${host}.cmake" <<EOF
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR ${sysproc})
set(CMAKE_C_COMPILER ${host}-gcc)
set(CMAKE_RC_COMPILER ${host}-windres)
set(CMAKE_FIND_ROOT_PATH /usr/${host})
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
EOF
  rm -rf "bld-${host}"; mkdir "bld-${host}"
  ( cd "bld-${host}"
    cmake "../openjpeg-${OPJ_VER}" \
      -DCMAKE_TOOLCHAIN_FILE="$OUT/tc-${host}.cmake" \
      -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$prefix" \
      -DBUILD_SHARED_LIBS=OFF -DBUILD_CODEC=OFF \
      -DBUILD_DOC=OFF -DBUILD_TESTING=OFF >/dev/null 2>&1
    make -j"$(nproc)" >/dev/null 2>&1
    make install >/dev/null 2>&1 )
  echo "   -> $(ls "$prefix"/lib/libopenjp2.a "$prefix"/lib/libz.a)"
}

build_one x86_64-w64-mingw32 "$OUT/out-x64" x86_64
build_one i686-w64-mingw32   "$OUT/out-x86" x86

echo ">> done. deps in $OUT/out-x64 and $OUT/out-x86"
