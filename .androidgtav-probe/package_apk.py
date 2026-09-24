#!/usr/bin/env python3
"""打包 GTA5 Android 诊断/修复 APK  ->  android_build/dist/gtav.apk

================================ 这个脚本在干什么 ================================

我们**不跑 gradle 全量构建**（那要几十分钟，且 java/资源/manifest 十几天没动）。
每次只改了引擎里的一两个 .cpp，所以只做“外科手术式”替换，分钟级完成：

  1. 用 llvm-ar 把重新编译出来的 .o 换进 libgtav_engine.a（只替换同名成员）
  2. 用 ndk-build 从这个归档重新链接出 libgtav.so
  3. 把上一版 APK 里的 lib/arm64-v8a/libgtav.so 单独抽换成新的，其余原样保留
  4. 用 apksigner 重新签名

产物永远叫 dist/gtav.apk（固定名、固定目录），不再是埋在 RB_zcode_* 里、
名字还带历史包袱的 gtav-pause-mouse-pointer.apk。

============================== 累积构建（重要） ==============================

每一版都是在上一版基础上叠加的。BASELINE_DIR 指向“上一版”的 integrated 目录，
它里面的 libgtav_engine.a + *.apk 就是这次的起点。
==> 每切一版新的累积基线，只需把下面 BASELINE_DIR 改成那一轮的 integrated 目录。

=============================== 怎么用 ===============================

先编译要改的 TU（列表相对 rage/base/src，游戏层会回退到 game/）：
    cd android_build
    CONFIG=release scripts/build_sys.sh <sources.txt> <round>/compiled

再打包（把编出来的 .o 传进来，可传多个）：
    python package_apk.py <round>/compiled/audio_cutsceneaudioentity.o

装机：
    adb -s 192.168.0.11:5555 install --no-incremental -r dist/gtav.apk

前置条件：X: 必须 subst 到本仓库根（subst X: "E:\\GTAV Source"），
ndk-build 靠短路径规避 Windows 路径长度限制。
"""
from pathlib import Path
import argparse
import hashlib
import json
import os
import subprocess
import sys
import zipfile

# ------------------------------------------------------------------ 固定路径
BUILD = Path(__file__).resolve().parent                 # .../dev_ng/android_build
SDK = Path(os.environ.get('LOCALAPPDATA', str(Path.home() / 'AppData/Local'))) / 'Android/Sdk'
NDK = SDK / 'ndk/29.0.14206865'
BIN = NDK / 'toolchains/llvm/prebuilt/windows-x86_64/bin'
AR = BIN / 'llvm-ar.exe'
STRINGS = BIN / 'llvm-strings.exe'
READELF = BIN / 'llvm-readelf.exe'
SIGNER = SDK / 'build-tools/37.0.0/apksigner.bat'
KEYSTORE = Path.home() / '.android/debug.keystore'
BUILD_X = Path('X:/gta5/src/dev_ng/android_build')      # subst X: <workspace root>

# ------------------------------------------------------------ 当前累积基线
# 上一版的 integrated 目录。切新基线时改这一行即可。
BASELINE_DIR = BUILD / 'RB_zcode_audiosched_0919/integrated'

# 最终产物固定落这里
DIST = BUILD / 'dist'
WORK = DIST / '_work'                                    # 中间产物（归档/.so/obj），可随时删

BUILD_ENV = os.environ.copy()
BUILD_ENV.setdefault('OS', 'Windows_NT')
BUILD_ENV.setdefault('PROCESSOR_ARCHITECTURE', 'AMD64')
BUILD_ENV.setdefault('JAVA_HOME', 'C:/Program Files/Android/Android Studio/jbr')


def sha(path):
    digest = hashlib.sha256()
    with Path(path).open('rb') as stream:
        for block in iter(lambda: stream.read(1 << 20), b''):
            digest.update(block)
    return digest.hexdigest()


def run(args, log, cwd=None):
    result = subprocess.run([str(x) for x in args], cwd=cwd, env=BUILD_ENV,
                            capture_output=True, text=True, errors='replace')
    with Path(log).open('a', encoding='utf-8') as fh:
        fh.write('$ ' + subprocess.list2cmdline([str(x) for x in args]) + '\n')
        fh.write(result.stdout + result.stderr + '\n')
    if result.returncode:
        print((result.stdout + result.stderr)[-8000:])
        raise SystemExit(f'命令失败({result.returncode}): {subprocess.list2cmdline([str(x) for x in args])}')
    return result.stdout


def baseline_apk(baseline_dir):
    """基线目录里挑那个已签名的 apk（排除 unsigned.apk）。"""
    apks = [p for p in baseline_dir.glob('*.apk') if p.name != 'unsigned.apk']
    if len(apks) != 1:
        raise SystemExit(f'基线目录里应恰好有一个已签名 apk，实际找到: {[p.name for p in apks]}')
    return apks[0]


def main():
    ap = argparse.ArgumentParser(description='打包 dist/gtav.apk（.so 热替换 + 重签名）')
    ap.add_argument('objects', nargs='+',
                    help='重编出的 .o，形如 path 或 member=path（member 缺省取文件名，须与归档成员同名）')
    ap.add_argument('--baseline', type=Path, default=BASELINE_DIR,
                    help=f'上一版累积基线的 integrated 目录（默认 {BASELINE_DIR.name}）')
    ap.add_argument('--dex', type=Path, default=None,
                    help='重编出的 classes.dex；改了 java 层时传入，热替换基线 APK 里的 classes.dex')
    args = ap.parse_args()

    baseline = args.baseline.resolve()
    base_archive = baseline / 'libgtav_engine.a'
    base_apk = baseline_apk(baseline)
    if not base_archive.is_file():
        raise SystemExit(f'基线归档不存在: {base_archive}')

    dex = None
    if args.dex is not None:
        dex = args.dex.resolve()
        if dex.suffix != '.dex' or not dex.is_file() or not dex.stat().st_size:
            raise SystemExit(f'无效的 classes.dex: {dex}')

    # X: 必须指向本仓库，否则 ndk-build 路径不对
    if not BUILD_X.exists() or not BUILD_X.samefile(BUILD):
        raise SystemExit('X: 未映射到本仓库根。先执行: subst X: "E:\\GTAV Source"')

    # 解析替换对象：member 名（=归档成员名）-> 磁盘上的 .o
    replacements = {}
    for arg in args.objects:
        member, sep, source = arg.partition('=')
        path = Path(source if sep else arg).resolve()
        name = member if sep else path.name
        if path.suffix != '.o' or not path.is_file() or not path.stat().st_size:
            raise SystemExit(f'无效的 .o: {path}')
        if name in replacements:
            raise SystemExit(f'重复的替换成员: {name}')
        replacements[name] = path

    # 干净的输出目录
    DIST.mkdir(exist_ok=True)
    if WORK.exists():
        import shutil
        shutil.rmtree(WORK)
    WORK.mkdir(parents=True)
    log = DIST / 'build.log'
    log.write_text('', encoding='utf-8')

    # 打包前后对“只读输入”做哈希校验，确保没有被意外改写
    protected = [base_archive, base_apk]
    before = {str(p): sha(p) for p in protected}

    # 1) 列出基线归档成员，把要替换的换成新的 .o（按文件名匹配）
    members = run([AR, 't', base_archive], log).splitlines()
    selected, found = [], set()
    for member in members:
        p = Path(member)
        if not p.is_absolute():
            p = baseline / p
        if p.name in replacements:
            found.add(p.name)
            p = replacements[p.name]
        if not p.is_file():
            raise SystemExit(f'归档成员对应文件缺失: {p}')
        selected.append(p.resolve())
    if found != set(replacements):
        raise SystemExit(f'这些成员在基线归档里根本不存在，名字对不上: {set(replacements) - found}')
    if len({p.name for p in selected}) != len(selected):
        raise SystemExit('归档里出现同名成员，无法唯一替换')

    # 2) 重建归档
    archive = WORK / 'libgtav_engine.a'
    for i in range(0, len(selected), 80):
        run([AR, 'rcsT', archive] + selected[i:i + 80], log)
    written = run([AR, 't', archive], log).splitlines()
    if len(written) != len(selected):
        raise SystemExit('重建后的归档成员数不符')

    # 3) 用 ndk-build 从归档重链 libgtav.so（走 X: 短路径）
    work_x = BUILD_X / WORK.relative_to(BUILD)
    mk = (BUILD / 'app/jni/Android.mk').read_text(encoding='utf-8')
    mk = mk.replace('LOCAL_PATH := $(call my-dir)',
                    f'LOCAL_PATH := {(BUILD_X / "app/jni").as_posix()}')
    mk = mk.replace('jni/../../libgtav_engine.a', (work_x / 'libgtav_engine.a').as_posix())
    (WORK / 'Android.mk').write_text(mk, encoding='utf-8')
    run(['cmd.exe', '/c', NDK / 'ndk-build.cmd', 'NDK_PROJECT_PATH=.',
         f'APP_BUILD_SCRIPT={work_x.as_posix()}/Android.mk', 'NDK_APPLICATION_MK=jni/Application.mk',
         f'NDK_OUT={(work_x / "obj").as_posix()}', f'NDK_LIBS_OUT={(work_x / "libs").as_posix()}', '-j4'],
        log, cwd=str(BUILD_X / 'app'))

    so = WORK / 'obj/local/arm64-v8a/libgtav.so'
    data = so.read_bytes()
    # 健全性：故事模式启动串必须还在（防止链错基线）
    if b'-DoReleaseStartup' not in data:
        raise SystemExit('libgtav.so 里找不到故事启动串，基线可能链错了')

    # 4) 把新 .so（改了 java 层时连同 classes.dex）抽换进基线 APK，
    #    其余条目原样拷贝，丢掉旧签名。
    dex_bytes = dex.read_bytes() if dex is not None else None
    dex_seen = False
    unsigned = WORK / 'unsigned.apk'
    with zipfile.ZipFile(base_apk) as src, zipfile.ZipFile(unsigned, 'w') as dst:
        for info in src.infolist():
            if info.filename.upper().startswith('META-INF/'):
                continue
            if info.filename == 'lib/arm64-v8a/libgtav.so':
                payload = data
            elif dex_bytes is not None and info.filename == 'classes.dex':
                payload = dex_bytes
                dex_seen = True
            else:
                payload = src.read(info.filename)
            dst.writestr(info, payload)
    if dex_bytes is not None and not dex_seen:
        raise SystemExit('基线 APK 里没有 classes.dex 可替换，manifest 可能是 hasCode=false 的壳')

    # 5) 重新签名 -> dist/gtav.apk
    apk = DIST / 'gtav.apk'
    if apk.exists():
        apk.unlink()
    run([SIGNER, 'sign', '--ks', KEYSTORE, '--ks-pass', 'pass:android',
         '--out', apk, unsigned], log)
    verify = run([SIGNER, 'verify', '--verbose', apk], log)
    notes = run([READELF, '--notes', so], log)

    # 只读输入必须原封不动
    after = {p: sha(p) for p in before}
    if before != after:
        raise SystemExit('构建过程中只读输入被改写了')

    manifest = {
        'baseline_dir': str(baseline),
        'baseline_apk': str(base_apk),
        'baseline_apk_sha256': before[str(base_apk)],
        'objects': {name: {'path': str(p), 'sha256': sha(p)} for name, p in replacements.items()},
        'dex': {'path': str(dex), 'sha256': sha(dex)} if dex is not None else None,
        'apk': str(apk),
        'apk_sha256': sha(apk),
        'so_sha256': sha(so),
        'member_count': len(written),
    }
    (DIST / 'verification.json').write_text(json.dumps(manifest, indent=2, ensure_ascii=False),
                                            encoding='utf-8')
    (DIST / 'verification.txt').write_text(verify + notes, encoding='utf-8')

    print(notes)
    print('APK       ', apk)
    print('APK  SHA256', manifest['apk_sha256'])
    print('.so  SHA256', manifest['so_sha256'])
    print('install   : adb -s 192.168.0.11:5555 install --no-incremental -r', apk)


if __name__ == '__main__':
    main()
