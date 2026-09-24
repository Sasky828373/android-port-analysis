#!/usr/bin/env bash
set -euo pipefail

DXVK_TAG="${DXVK_TAG:-v3.1.1}"
SDL_TAG="${SDL_TAG:-release-3.4.16}"
ANDROID_API="${ANDROID_API:-26}"
ANDROID_ABI="${ANDROID_ABI:-arm64-v8a}"
ROOT="${ROOT:-$PWD/.dxvk-android-build}"
PREFIX="$ROOT/prefix"
OUT="$ROOT/out"

: "${ANDROID_NDK_HOME:?ANDROID_NDK_HOME must point to an Android NDK}"

HOST_TAG="linux-x86_64"
TOOLCHAIN="$ANDROID_NDK_HOME/toolchains/llvm/prebuilt/$HOST_TAG"
CC="$TOOLCHAIN/bin/aarch64-linux-android${ANDROID_API}-clang"
CXX="$TOOLCHAIN/bin/aarch64-linux-android${ANDROID_API}-clang++"

rm -rf "$ROOT"
mkdir -p "$ROOT" "$PREFIX" "$OUT"

echo "==> Building SDL3 $SDL_TAG for Android ARM64"
git clone --depth 1 --branch "$SDL_TAG" https://github.com/libsdl-org/SDL.git "$ROOT/SDL"
cmake -S "$ROOT/SDL" -B "$ROOT/sdl-build" -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE="$ANDROID_NDK_HOME/build/cmake/android.toolchain.cmake" \
  -DANDROID_ABI="$ANDROID_ABI" \
  -DANDROID_PLATFORM="android-$ANDROID_API" \
  -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_SHARED_LIBS=OFF \
  -DSDL_SHARED=OFF \
  -DSDL_STATIC=ON \
  -DSDL_TESTS=OFF \
  -DSDL_EXAMPLES=OFF \
  -DSDL_INSTALL=ON \
  -DCMAKE_INSTALL_PREFIX="$PREFIX"
ninja -C "$ROOT/sdl-build"
ninja -C "$ROOT/sdl-build" install

echo "==> Cloning DXVK $DXVK_TAG"
git clone --recursive --depth 1 --branch "$DXVK_TAG" https://github.com/doitsujin/dxvk.git "$ROOT/dxvk"

echo "==> Applying minimal Android-native build patch"
python3 - "$ROOT/dxvk/meson.build" <<'PY'
from pathlib import Path
import sys
p = Path(sys.argv[1])
s = p.read_text()
old = """  link_args += [
    '-static-libgcc',
    '-static-libstdc++',
  ]
"""
new = """  if platform != 'android'
    link_args += [
      '-static-libgcc',
      '-static-libstdc++',
    ]
  endif
"""
if old not in s:
    raise SystemExit("Expected DXVK native link block not found")
p.write_text(s.replace(old, new, 1))
PY

cat > "$ROOT/android-arm64.ini" <<EOF
[binaries]
c = '$CC'
cpp = '$CXX'
ar = '$TOOLCHAIN/bin/llvm-ar'
strip = '$TOOLCHAIN/bin/llvm-strip'
ranlib = '$TOOLCHAIN/bin/llvm-ranlib'
pkg-config = 'pkg-config'
cmake = 'cmake'

[host_machine]
system = 'android'
cpu_family = 'aarch64'
cpu = 'aarch64'
endian = 'little'

[properties]
needs_exe_wrapper = true

[built-in options]
c_args = ['-O3', '-fPIC', '-ffunction-sections', '-fdata-sections']
cpp_args = ['-O3', '-fPIC', '-ffunction-sections', '-fdata-sections']
c_link_args = ['-Wl,--gc-sections']
cpp_link_args = ['-Wl,--gc-sections']
EOF

# NDK clang carries its own sysroot. Do not let Meson/pkg-config prepend it
# to absolute headers installed in our target prefix.
export PKG_CONFIG_PATH="$PREFIX/lib/pkgconfig:$PREFIX/share/pkgconfig"
export PKG_CONFIG_LIBDIR="$PKG_CONFIG_PATH"
unset PKG_CONFIG_SYSROOT_DIR

echo "==> Configuring DXVK Native for Android ARM64"
meson setup "$ROOT/dxvk-build" "$ROOT/dxvk" \
  --cross-file "$ROOT/android-arm64.ini" \
  --buildtype release \
  --strip \
  -Db_lto=true \
  -Db_ndebug=true \
  -Dnative_sdl3=enabled \
  -Dnative_sdl2=disabled \
  -Dnative_glfw=disabled \
  -Denable_dxgi=true \
  -Denable_d3d11=true \
  -Denable_d3d10=false \
  -Denable_d3d9=false \
  -Denable_d3d8=false

echo "==> Building"
ninja -C "$ROOT/dxvk-build" -v

echo "==> Collecting native Android libraries"
find "$ROOT/dxvk-build" -type f \( -name 'libdxvk_d3d11.so*' -o -name 'libdxvk_dxgi.so*' \) -print -exec cp -f {} "$OUT/" \;

D3D11_FILE="$(find "$OUT" -maxdepth 1 -type f -name 'libdxvk_d3d11.so*' | head -n1 || true)"
DXGI_FILE="$(find "$OUT" -maxdepth 1 -type f -name 'libdxvk_dxgi.so*' | head -n1 || true)"
test -n "$D3D11_FILE"
test -n "$DXGI_FILE"
cp -f "$D3D11_FILE" "$OUT/libdxvk_d3d11.so"
cp -f "$DXGI_FILE" "$OUT/libdxvk_dxgi.so"

"$TOOLCHAIN/bin/llvm-readelf" -h "$OUT/libdxvk_d3d11.so"
"$TOOLCHAIN/bin/llvm-readelf" -h "$OUT/libdxvk_dxgi.so"
"$TOOLCHAIN/bin/llvm-readelf" -d "$OUT/libdxvk_d3d11.so" | tee "$OUT/d3d11-dynamic.txt"
"$TOOLCHAIN/bin/llvm-readelf" -d "$OUT/libdxvk_dxgi.so" | tee "$OUT/dxgi-dynamic.txt"

cat > "$OUT/BUILD-INFO.txt" <<EOF
DXVK=$DXVK_TAG
SDL=$SDL_TAG
ABI=$ANDROID_ABI
API=$ANDROID_API
WSI=SDL3 Android
Components=D3D11,DXGI
Optimization=Release,O3,LTO,gc-sections,strip
EOF

echo "==> Done"
ls -lah "$OUT"
