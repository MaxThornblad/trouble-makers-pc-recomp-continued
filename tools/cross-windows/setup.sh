#!/bin/bash
# Prepares the MinGW cross build of troublemakers.exe on Ubuntu 24.04.
# Run from the repo root after the normal setup (README "One-time setup").
# Needs (ask the owner before installing): apt packages
#   g++-mingw-w64-x86-64-posix gcc-mingw-w64-x86-64-posix binutils-mingw-w64-x86-64 wine
# and the SDL2 MinGW development archive (official GitHub release, pinned below).
set -euo pipefail
REPO=$(pwd)
X=${TM_XDEPS:-/home/claude/xdeps}
mkdir -p "$X/hosttools" "$X/winshim/GL" "$X/winshim/gl"
cp tools/cross-windows/mingw_compat.h tools/cross-windows/mingw_d3d12_guids.h tools/cross-windows/mingw_dxc_uuids.h "$X/"
cp tools/cross-windows/dxc-wine "$X/hosttools/"

# Case-sensitive header names MSVC code uses, pointing at MinGW's.
M=/usr/x86_64-w64-mingw32/include
for p in BaseTsd.h:basetsd.h Inspectable.h:inspectable.h ShellScalingAPI.h:shellscalingapi.h \
         Shlobj.h:shlobj.h Shlwapi.h:shlwapi.h Windows.h:windows.h; do
    ln -sf "$M/${p#*:}" "$X/winshim/${p%%:*}"
done
ln -sf "$M/GL/gl.h" "$X/winshim/GL/GL.h"
ln -sf "$M/GL/gl.h" "$X/winshim/gl/gl.h"
ln -sf "$M/GL/glu.h" "$X/winshim/gl/glu.h"

# RT64's build-time tool, built for the host.
c++ -O2 -std=c++17 lib/rt64/src/tools/file_to_c/file_to_c.cpp -o "$X/hosttools/file_to_c"

# DirectX-Headers (MinGW's d3d12.h is too old), pinned commit.
if [ ! -d "$X/DirectX-Headers" ]; then
    git clone -q https://github.com/microsoft/DirectX-Headers "$X/DirectX-Headers"
    git -C "$X/DirectX-Headers" checkout -q 461ef51
fi

# SDL2 2.30.3 MinGW dev archive (SDL2-devel-2.30.3-mingw.tar.gz from
# github.com/libsdl-org/SDL/releases), SHA-256 checked.
if [ ! -d "$X/SDL2-2.30.3" ]; then
    T=$(mktemp -d)
    curl -fsSL -o "$T/sdl2.tgz" https://github.com/libsdl-org/SDL/releases/download/release-2.30.3/SDL2-devel-2.30.3-mingw.tar.gz
    echo "e76d7742a1ae19957768b59199b70d9175c2ce7609e35fe7eeedd8eb54c1e2d5  $T/sdl2.tgz" | sha256sum -c -
    tar xzf "$T/sdl2.tgz" -C "$X"
fi

# RT64's MinGW changes (on top of patches/rt64).
git -C lib/rt64 apply --check "$REPO/tools/cross-windows/rt64-mingw.patch" 2>/dev/null \
    && git -C lib/rt64 apply "$REPO/tools/cross-windows/rt64-mingw.patch" || echo "rt64-mingw.patch already applied?"

# A Wine prefix for dxc.exe.
export WINEPREFIX=${TM_WINEPREFIX:-/tmp/claude-0/wt/prefix} WINEDEBUG=-all
mkdir -p "$WINEPREFIX"; wineboot -i >/dev/null 2>&1 || true

cmake -B build_win -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_TOOLCHAIN_FILE="$REPO/tools/cross-windows/mingw-toolchain.cmake" \
    -DSDL2_DIR="$X/SDL2-2.30.3/x86_64-w64-mingw32/lib/cmake/SDL2" \
    -DRT64_DIRECTX_HEADERS="$X/DirectX-Headers" -DRT64_MINGW_COMPAT_DIR="$X" \
    -DRT64_HOST_DXC="$X/hosttools/dxc-wine" -DRT64_HOST_FILE_TO_C="$X/hosttools/file_to_c"
echo "configured: cmake --build build_win --target troublemakers"
