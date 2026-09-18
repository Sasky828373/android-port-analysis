# DXVK Native Android ARM64 bring-up

First target: reproduce a clean Android arm64 DXVK Native D3D11/DXGI pair before adding GTA/A840 performance patches.

Requirements:
- Android NDK
- Android arm64 SDL2 install prefix containing sdl2.pc
- Meson, Ninja, pkg-config, glslang
- DXVK source checkout with submodules

Run:
`ANDROID_NDK_HOME=... SDL2_PREFIX=... DXVK_DIR=... bash scripts/build-dxvk-android-arm64.sh`

Validation gate: both D3D11 and DXGI outputs must be AArch64 ELF and include the SDL2 WSI. Do not replace old31 until this clean build launches in GTA V.
