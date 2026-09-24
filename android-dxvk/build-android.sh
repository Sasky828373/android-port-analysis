#!/usr/bin/env bash
set -euo pipefail

DXVK_TAG="${DXVK_TAG:-v3.1.1}"
ANDROID_API="${ANDROID_API:-26}"
ANDROID_ABI="${ANDROID_ABI:-arm64-v8a}"
ROOT="${ROOT:-$PWD/.dxvk-android-build}"
OUT="$ROOT/out"

: "${ANDROID_NDK_HOME:?ANDROID_NDK_HOME must point to an Android NDK}"

HOST_TAG="linux-x86_64"
TOOLCHAIN="$ANDROID_NDK_HOME/toolchains/llvm/prebuilt/$HOST_TAG"
CC="$TOOLCHAIN/bin/aarch64-linux-android${ANDROID_API}-clang"
CXX="$TOOLCHAIN/bin/aarch64-linux-android${ANDROID_API}-clang++"

rm -rf "$ROOT"
mkdir -p "$ROOT" "$OUT"

echo "==> Cloning upstream DXVK $DXVK_TAG"
git clone --recursive --depth 1 --branch "$DXVK_TAG" https://github.com/doitsujin/dxvk.git "$ROOT/dxvk"

echo "==> Installing direct Android WSI"
mkdir -p "$ROOT/dxvk/src/wsi/android" "$ROOT/dxvk/include/native/wsi"
cp android-dxvk/android-wsi/native_android.h "$ROOT/dxvk/include/native/wsi/native_android.h"
cp android-dxvk/android-wsi/wsi_platform_android.h "$ROOT/dxvk/src/wsi/android/wsi_platform_android.h"
cp android-dxvk/android-wsi/wsi_platform_android.cpp "$ROOT/dxvk/src/wsi/android/wsi_platform_android.cpp"

python3 - "$ROOT/dxvk" <<'PY'
from pathlib import Path
import sys

root = Path(sys.argv[1])

# Main Meson: Android is its own native WSI and must not require SDL/GLFW.
p = root / "meson.build"
s = p.read_text()
s = s.replace(
"""  lib_sdl3 = dependency('sdl3', required: get_option('native_sdl3'))
  lib_sdl2 = dependency('sdl2', required: get_option('native_sdl2'))
  lib_glfw = dependency('glfw3', required: get_option('native_glfw'))
  if lib_sdl3.found()
    compiler_args += ['-DDXVK_WSI_SDL3']
  endif
  if lib_sdl2.found()
    compiler_args += ['-DDXVK_WSI_SDL2']
  endif
  if lib_glfw.found()
    compiler_args += ['-DDXVK_WSI_GLFW']
  endif
  if (not lib_sdl3.found() and not lib_sdl2.found() and not lib_glfw.found())
    error('SDL3, SDL2, or GLFW are required to build dxvk-native')
  endif
""",
"""  if platform == 'android'
    lib_sdl3 = dependency('', required: false)
    lib_sdl2 = dependency('', required: false)
    lib_glfw = dependency('', required: false)
    compiler_args += ['-DDXVK_WSI_ANDROID', '-DVK_USE_PLATFORM_ANDROID_KHR']
  else
    lib_sdl3 = dependency('sdl3', required: get_option('native_sdl3'))
    lib_sdl2 = dependency('sdl2', required: get_option('native_sdl2'))
    lib_glfw = dependency('glfw3', required: get_option('native_glfw'))
    if lib_sdl3.found()
      compiler_args += ['-DDXVK_WSI_SDL3']
    endif
    if lib_sdl2.found()
      compiler_args += ['-DDXVK_WSI_SDL2']
    endif
    if lib_glfw.found()
      compiler_args += ['-DDXVK_WSI_GLFW']
    endif
    if (not lib_sdl3.found() and not lib_sdl2.found() and not lib_glfw.found())
      error('SDL3, SDL2, or GLFW are required to build dxvk-native')
    endif
  endif
""")

s = s.replace(
"""  link_args += [
    '-static-libgcc',
    '-static-libstdc++',
  ]
""",
"""  if platform != 'android'
    link_args += [
      '-static-libgcc',
      '-static-libstdc++',
    ]
  endif
""")
p.write_text(s)

# DXVK's Vulkan loader enables Win32 WSI unconditionally. Disable that define
# for Android so Vulkan headers expose Android, not Win32, surface entry points.
p = root / "src/vulkan/vulkan_loader.h"
s = p.read_text()
s = s.replace(
"""#define VK_USE_PLATFORM_WIN32_KHR 1
#include <vulkan/vulkan.h>
""",
"""#if !defined(__ANDROID__)
#define VK_USE_PLATFORM_WIN32_KHR 1
#endif
#include <vulkan/vulkan.h>
""")
p.write_text(s)

# Native handle selection.
p = root / "include/native/wsi/native_wsi.h"
s = p.read_text()
s = s.replace(
"""#ifdef DXVK_WSI_WIN32
#error You shouldnt be using this code path.
#elif DXVK_WSI_SDL3
""",
"""#ifdef DXVK_WSI_WIN32
#error You shouldnt be using this code path.
#elif DXVK_WSI_ANDROID
#include "wsi/native_android.h"
#elif DXVK_WSI_SDL3
""")
p.write_text(s)

# Register Android bootstrap.
p = root / "src/wsi/wsi_platform.h"
s = p.read_text()
s = s.replace(
"""#if defined(DXVK_WSI_WIN32)
  extern WsiBootstrap Win32WSI;
#endif
""",
"""#if defined(DXVK_WSI_WIN32)
  extern WsiBootstrap Win32WSI;
#endif
#if defined(DXVK_WSI_ANDROID)
  extern WsiBootstrap AndroidWSI;
#endif
""")
p.write_text(s)

p = root / "src/wsi/wsi_platform.cpp"
s = p.read_text()
s = s.replace(
"""#if defined(DXVK_WSI_WIN32)
    &Win32WSI,
#endif
""",
"""#if defined(DXVK_WSI_WIN32)
    &Win32WSI,
#endif
#if defined(DXVK_WSI_ANDROID)
    &AndroidWSI,
#endif
""")
s = s.replace(
"""#if defined(DXVK_WSI_WIN32)
        hint = "Win32";
#else
        throw DxvkError("DXVK_WSI_DRIVER environment variable unset");
#endif
""",
"""#if defined(DXVK_WSI_WIN32)
        hint = "Win32";
#elif defined(DXVK_WSI_ANDROID)
        hint = "Android";
#else
        throw DxvkError("DXVK_WSI_DRIVER environment variable unset");
#endif
""")
p.write_text(s)

# Build Android backend and avoid SDL/GLFW dependency plumbing on Android.
p = root / "src/wsi/meson.build"
s = p.read_text()
s = s.replace(
"""  'win32/wsi_window_win32.cpp',
""",
"""  'win32/wsi_window_win32.cpp',
  'android/wsi_platform_android.cpp',
""")
s = s.replace(
"""else
  wsi_deps += [
    lib_sdl3.partial_dependency(compile_args: true, includes: true),
    lib_sdl2.partial_dependency(compile_args: true, includes: true),
    lib_glfw.partial_dependency(compile_args: true, includes: true),
  ]
endif
""",
"""elif platform != 'android'
  wsi_deps += [
    lib_sdl3.partial_dependency(compile_args: true, includes: true),
    lib_sdl2.partial_dependency(compile_args: true, includes: true),
    lib_glfw.partial_dependency(compile_args: true, includes: true),
  ]
endif
""")
p.write_text(s)
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

echo "==> Configuring DXVK Native: direct Android WSI / D3D11 + DXGI"
meson setup "$ROOT/dxvk-build" "$ROOT/dxvk" \
  --cross-file "$ROOT/android-arm64.ini" \
  --buildtype release \
  --strip \
  -Db_lto=true \
  -Db_ndebug=true \
  -Dnative_sdl3=disabled \
  -Dnative_sdl2=disabled \
  -Dnative_glfw=disabled \
  -Denable_dxgi=true \
  -Denable_d3d11=true \
  -Denable_d3d10=false \
  -Denable_d3d9=false \
  -Denable_d3d8=false

echo "==> Building"
ninja -C "$ROOT/dxvk-build" -v

echo "==> Collecting Android ARM64 libraries"
D3D11_FILE="$ROOT/dxvk-build/src/d3d11/libdxvk_d3d11.so"
DXGI_FILE="$ROOT/dxvk-build/src/dxgi/libdxvk_dxgi.so"
test -f "$D3D11_FILE"
test -f "$DXGI_FILE"
cp -f "$D3D11_FILE" "$OUT/libdxvk_d3d11.so"
cp -f "$DXGI_FILE" "$OUT/libdxvk_dxgi.so"

LIBCXX_FILE="$TOOLCHAIN/sysroot/usr/lib/aarch64-linux-android/libc++_shared.so"
test -f "$LIBCXX_FILE"
cp -f "$LIBCXX_FILE" "$OUT/libc++_shared.so"

"$TOOLCHAIN/bin/llvm-strip" --strip-unneeded "$OUT/libdxvk_d3d11.so"
"$TOOLCHAIN/bin/llvm-strip" --strip-unneeded "$OUT/libdxvk_dxgi.so"

"$TOOLCHAIN/bin/llvm-readelf" -h "$OUT/libdxvk_d3d11.so"
"$TOOLCHAIN/bin/llvm-readelf" -h "$OUT/libdxvk_dxgi.so"
"$TOOLCHAIN/bin/llvm-readelf" -d "$OUT/libdxvk_d3d11.so" | tee "$OUT/d3d11-dynamic.txt"
"$TOOLCHAIN/bin/llvm-readelf" -d "$OUT/libdxvk_dxgi.so" | tee "$OUT/dxgi-dynamic.txt"

cat > "$OUT/BUILD-INFO.txt" <<EOF
DXVK=$DXVK_TAG
ABI=$ANDROID_ABI
API=$ANDROID_API
WSI=Direct-ANativeWindow
VulkanSurface=VK_KHR_android_surface
Components=D3D11,DXGI
Runtime=NDK-r29-libc++_shared
Optimization=Release,O3,LTO,gc-sections,explicit-strip
SDL=none
EOF

echo "==> Done"
ls -lah "$OUT"
