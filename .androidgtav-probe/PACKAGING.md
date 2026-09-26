# 打包 Android APK（人类看这份就够了）

## TL;DR

```bash
# 0) 一次性：把 X: 映射到仓库根（ndk-build 靠短路径）
subst X: "E:\GTAV Source"

# 1) 编译你改过的 TU（列表路径相对 rage/base/src，游戏层自动回退到 game/）
cd "E:/GTAV Source/gta5/src/dev_ng/android_build"
CONFIG=release scripts/build_sys.sh <round>/sources.txt <round>/compiled

# 2) 打包成 dist/gtav.apk（把上一步编出的 .o 传进去，可多个）
python package_apk.py <round>/compiled/<your>.o

# 3) 装机
adb -s 192.168.0.11:5555 install --no-incremental -r dist/gtav.apk
```

产物固定是 **`android_build/dist/gtav.apk`**，附带 `dist/verification.json`
（记录基线、替换了哪些 .o、APK/.so 的 SHA256）和 `dist/build.log`。

## 为什么不跑 gradle

一次完整 gradle 构建要重编所有 native + 全量打包资源/java，几十分钟；而每轮改动
只碰一两个引擎 .cpp。所以走“外科手术式”热替换，分钟级：

1. `llvm-ar` 把重编的 `.o` 换进 `libgtav_engine.a`（按成员名替换）
2. `ndk-build` 从归档重链 `libgtav.so`
3. 把上一版 APK 里的 `lib/arm64-v8a/libgtav.so` 单独换成新的，其余条目原样保留
4. `apksigner` 重新签名

只有改了 **java / AndroidManifest / 资源 / 包名 / 签名 keystore** 时才需要重跑
gradle 出一个新的“壳 APK”。壳没变时重跑纯属浪费（产出逐位相同）。

## 累积构建

每一版都叠在上一版之上。`package_apk.py` 顶部的 `BASELINE_DIR` 指向“上一版”的
`integrated` 目录（它的 `libgtav_engine.a` + `*.apk` 就是本次起点）。
**切新基线时，只改 `BASELINE_DIR` 那一行即可**，或用 `--baseline <dir>` 覆盖。

当前基线链最新是：`RB_zcode_audiosched_0919/integrated`
（累积内容：4 个崩溃修复 + PCM + ADPCM 软解修复 + 4 个 UI 修复 + 混音线程 SCHED_FIFO）。

## 历史遗留

老流程把产物埋在 `RB_zcode_*/integrated/` 下，还叫 `gtav-pause-mouse-pointer.apk`
（因为最早写全签名流程的是那一轮，之后所有轮 `importlib` 复用它、没改输出名）。
`package_apk.py` 是自包含的干净版，取代那套 `importlib` 链式加载，统一出 `dist/gtav.apk`。
旧的 `RB_zcode_*/build_apk.py` 仍可用，但新工作一律用 `package_apk.py`。
