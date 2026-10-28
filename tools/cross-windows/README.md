# Windows build from Linux (MinGW-w64)

How the co-op update packages (`TroubleMakers-coop-update-N.zip`) are built in
a Linux container. The supported Windows build is still clang-cl on Windows
(main README); this is the cross build an agent without Windows uses.

1. Normal setup first (ROM, N64ModernRuntime patches, RT64 at 23cab603 with
   `patches/rt64/*.patch`, `N64Recomp` + `RSPRecomp` run).
2. `tools/cross-windows/setup.sh` (installs nothing itself; see its header for
   the apt packages, which need the owner's OK). It fills `$TM_XDEPS`
   (default `/home/claude/xdeps`), applies `rt64-mingw.patch` to `lib/rt64`
   and configures `build_win`.
3. `cmake --build build_win --target troublemakers -j2` (about 30 to 60 min
   from clean on 2 cores; incremental builds are minutes).

What the pieces are for:

- `mingw-toolchain.cmake`: posix-thread MinGW compilers, SDL2's MinGW package.
- `mingw_compat.h` (force-included everywhere): SAL macros and
  `SetThreadDescription` MinGW 11 lacks.
- `mingw_d3d12_guids.h`, `mingw_dxc_uuids.h`: interface IDs for GCC, which
  ignores `__declspec(uuid)`.
- `winshim/` (made by setup.sh): `Windows.h`, `Shlwapi.h` etc. under the
  capitalised names MSVC code includes.
- `dxc-wine`: RT64's shaders must be compiled by the Windows `dxc.exe` that
  matches the bundled `dxcompiler.dll` (D3D12 rejects DXIL libraries from
  another DXC build), run under Wine.
- `rt64-mingw.patch`: RT64 CMake for the cross build (host DXC and
  file_to_c, MinGW SDL2, DirectX-Headers), a `_WIN64` ctz fix and the
  out-parameter form of `GetCPUDescriptorHandleForHeapStart` (MinGW ABI).

Packaging: the update folder is `troublemakers.exe` plus `SDL2.dll`,
`dxcompiler.dll`, `dxil.dll` (both from `lib/rt64/src/contrib/dxc/bin/x64`),
`assets/` (both, with `SDL2.dll`, from `build_win/src/game/`), `steam_api64.dll`
(Steamworks.NET Standalone 2025.164.1, SHA-256
eb17909a76668cf9ae0b92a618a34a50f6c73d3a6787cb4dd8ce36a8b10bfb75),
`steam_appid.txt` (`480`) and a `README-update.txt`.
