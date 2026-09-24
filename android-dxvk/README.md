# DXVK 3.1.1 Native for Android ARM64

Independent Android build path based directly on upstream **doitsujin/dxvk v3.1.1**.
No XHYN DXVK binaries or source patches are used.

## First-stage target

- Android `arm64-v8a`
- Android API 26+
- DXVK Native
- D3D11 + DXGI only
- SDL3 Android WSI
- Release + O3 + LTO + dead-section elimination + strip
- Output:
  - `libdxvk_d3d11.so`
  - `libdxvk_dxgi.so`

SDL3 is intentionally used for the bootstrap build because upstream DXVK Native
already has a maintained SDL3 WSI. Once the clean ARM64 build is proven, the next
step is a dedicated Android WSI using `ANativeWindow` directly, followed by
Adreno/Turnip-specific profiling and tuning.

## Build

Set `ANDROID_NDK_HOME` and run:

```bash
chmod +x android-dxvk/build-android.sh
android-dxvk/build-android.sh
```

Artifacts are written to `.dxvk-android-build/out/`.

## Source versions

- DXVK: v3.1.1
- SDL3: release-3.4.16
- NDK CI: r29
