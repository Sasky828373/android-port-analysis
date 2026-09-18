#!/usr/bin/env bash
set -euo pipefail
: "${ANDROID_NDK_HOME:?Set ANDROID_NDK_HOME}"
DXVK_DIR="${DXVK_DIR:-$PWD/dxvk}"
API="${ANDROID_API:-29}"
HOST="${NDK_HOST:-linux-x86_64}"
TC="$ANDROID_NDK_HOME/toolchains/llvm/prebuilt/$HOST"
SDL2_PREFIX="${SDL2_PREFIX:?Set SDL2_PREFIX to the Android arm64 SDL2 prefix}"
CROSS="$DXVK_DIR/meson-android-arm64.ini"

cat > "$CROSS" <<EOF
[binaries]
c = '$TC/bin/aarch64-linux-android${API}-clang'
cpp = '$TC/bin/aarch64-linux-android${API}-clang++'
ar = '$TC/bin/llvm-ar'
strip = '$TC/bin/llvm-strip'
pkg-config = 'pkg-config'

[host_machine]
system = 'android'
cpu_family = 'aarch64'
cpu = 'armv8-a'
endian = 'little'

[properties]
sys_root = '$TC/sysroot'
EOF

export PKG_CONFIG_PATH="$SDL2_PREFIX/lib/pkgconfig"
export PKG_CONFIG_LIBDIR="$SDL2_PREFIX/lib/pkgconfig"
cd "$DXVK_DIR"
rm -rf build-android-arm64
meson setup build-android-arm64 --cross-file "$CROSS" -Dbuildtype=release -Dstrip=true -Dnative_sdl2=enabled -Dnative_sdl3=disabled -Dnative_glfw=disabled
ninja -C build-android-arm64 -j"$(nproc)"

echo "=== candidate outputs ==="
find build-android-arm64 -type f \( -name 'libdxvk_d3d11.so*' -o -name 'libdxvk_dxgi.so*' \) -print
echo "=== verify ==="
find build-android-arm64 -type f \( -name 'libdxvk_d3d11.so*' -o -name 'libdxvk_dxgi.so*' \) -exec file {} \;
find build-android-arm64 -type f \( -name 'libdxvk_d3d11.so*' -o -name 'libdxvk_dxgi.so*' \) -exec sh -c 'strings "$1" | grep -m1 -E "Sdl2WsiDriver|SDL2" || true' _ {} \;
