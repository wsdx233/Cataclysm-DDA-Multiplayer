# CDDA 联机 fork 原版构建基线

## 1. 目的

在修改运行模式、网络协议或玩家状态之前，先证明同一个提交能够稳定生成三类原版产物：

- Windows x64 MSVC 图形客户端，SDL3、tiles 和 sound 均启用。
- Android arm64 图形客户端 APK。
- Linux x64 curses 包，作为后续 headless server target 的构建前身。

Linux curses 包不是服务器，也不能作为多人服务运行。它只验证当前无 SDL 构建和打包路径没有被后续改动破坏。

初始上游基线固定为 `d84b90dd2aee090ca28c8dad5cdf1fab6dea151a`。

## 2. CI 入口

工作流位于 `.github/workflows/multiplayer-baseline.yml`，在以下情况运行：

- 手工触发 `workflow_dispatch`。
- 推送到 `multiplayer/main` 且构建相关文件发生变化。
- 面向 `multiplayer/main` 的 pull request 修改构建相关文件。

工作流不创建 GitHub Release，不需要 Android keystore，也不会使用正式发布凭据。每个产物旁边包含构建 manifest、源码提交和 SHA-256。

完整 `.po` 不在 Git 仓库中，而是在上游发布时从 Transifex 拉取。基线 workflow 不依赖 fork 私有的 Transifex token；它会校验并从固定的官方基线包提取已编译 `.mo`，供三端打包使用。

独立的 `.github/workflows/multiplayer-transport-spike.yml` 只构建
`tools/multiplayer/transport_spike/`：Linux 原生 GCC/Clang、Windows 原生 MSVC 执行 loopback CTest，
Android NDK arm64 执行交叉编译和 ELF/ABI 检查。它不链接游戏目标，也不构成生产 transport 或 TLS。

## 3. 固定依赖

| 依赖 | 固定值 |
| --- | --- |
| 上游源码基线 | `d84b90dd2aee090ca28c8dad5cdf1fab6dea151a` |
| CDDA-Tilesets | `13b1e25e4362af9e90885b323296ec8226ccae47` |
| CDDA-Soundpacks | `88586bb600eae4b053a8928bddbd5371d0b1ecc6` |
| vcpkg | `f6672d8e480ccdecddfad3fd1b838ba369ffe6cd` |
| 官方翻译来源包 SHA-256 | `999419a0c0da8ec25f92dd85127fe858145ff5b59e5582ad5094ad8a98c735ab` |
| Windows runner | `windows-2022` |
| Linux/Android runner | `ubuntu-24.04` |
| Windows CMake | `3.31.6` |
| Android JDK | `17` |
| Android platform | `35` |
| Android Build Tools | `34.0.0` |
| Android NDK | `28.1.13356709` |
| standalone Asio | `1.38.1` / `asio-1-38-1` |
| Asio commit | `dfd7b3e3145bac5d0e91a99fde69c6ae1442f971` |
| Asio archive SHA-256 | `2827b229972be80cdb14e5497962fa393d1adf036b5869e2b9c99f644daadacc` |

SDL3 Android AAR 的版本与 SHA-256 继续由 `android/app/build.gradle` 固定。桌面 shader compiler 继续复用 `.github/actions/build-sdl3-shaders` 中固定的 SDL_shadercross 提交。

## 4. 产物

### Linux

- `cdda-linux-curses-x64-baseline.tar.gz`
- `linux-build-manifest.txt`
- `linux-version.txt`

CI 会解压 tarball 并执行 `cataclysm --version`。

### Windows

- `cdda-windows-client-x64-baseline.zip`
- `windows-build-manifest.txt`
- `windows-version.txt`

CI 会执行打包目录中的 `cataclysm-tiles.exe --version`，并使用 `7z t` 检查压缩包。

### Android

- `cdda-android-arm64-baseline-unsigned.apk`
- `android-build-manifest.txt`
- `android-badging.txt`

该 APK 是 release 配置但未签名，适合验证构建内容，不用于安装或发布。CI 会检查 APK 完整性、包信息、
`arm64-v8a/libmain.so`、`android.permission.INTERNET`，并确认没有混入 32 位 ARM 或 x86 库。

## 5. 本地 Linux 环境

Ubuntu 24.04 推荐安装：

```bash
sudo apt-get update
sudo apt-get install -y \
  build-essential clang-18 cmake ccache gettext \
  libbz2-dev libncurses-dev ninja-build pkg-config zlib1g-dev
```

检查环境：

```bash
source build-scripts/activate-multiplayer-build-env.sh
./build-scripts/check-multiplayer-build-env.sh linux
```

激活脚本也支持无 root 的本地工具链目录 `~/.local/toolchains/cdda-linux`。该目录存在时，可以使用其中的 GCC 13、Make、gettext、ncurses、zlib 和 bzip2 开发文件；正式 CI 仍使用 Ubuntu runner 的 Clang 18 系统包。

本地快速构建不要求 libbacktrace，可使用：

```bash
make -j"$(nproc)" \
  COMPILER=g++-13 \
  TILES=0 SOUND=0 RELEASE=1 LOCALIZE=0 \
  BACKTRACE=0 PCH=0

./cataclysm --version
```

Git 仓库只带翻译 placeholder，因此本地快速构建关闭 localization。CI 的发布基线会额外固定并构建 libbacktrace、执行 `bindist`，再注入固定官方基线包中的完整 `.mo`。

## 6. 本地 Android 环境

需要 JDK 17、Android command-line tools、platform 35 和 NDK `28.1.13356709`：

```bash
source build-scripts/activate-multiplayer-build-env.sh

sdkmanager \
  "platform-tools" \
  "platforms;android-35" \
  "build-tools;34.0.0" \
  "cmake;3.22.1" \
  "ndk;28.1.13356709"
```

激活脚本默认使用用户目录中的 `~/.local/opt/temurin-17` 和 `~/Android/Sdk`，不会修改 `.bashrc`。也可以在 source 前设置 `JAVA_HOME` 或 `ANDROID_HOME` 覆盖默认值。

检查环境：

```bash
./build-scripts/check-multiplayer-build-env.sh android
```

构建 arm64 debug APK：

```bash
cd android
./gradlew \
  -Pj="$(nproc)" \
  -Pabi_arm_32=false \
  -Pabi_arm_64=true \
  -Pabi_x86_32=false \
  -Pabi_x86_64=false \
  -Plocalize=false \
  assembleExperimentalDebug
```

本地 debug APK 使用 Android 默认 debug keystore，可以直接安装。`-Plocalize=false` 让本地 smoke build 不依赖系统 gettext/make；CI 仍会构建全部翻译。CI 产出的 unsigned release APK 只用于构建基线。

## 7. 本地 Windows 环境

使用 Windows 11、Visual Studio 2022、Desktop development with C++、Game development with C++、CMake 3.31.x、MSYS2 gettext/make，以及固定 revision 的 vcpkg。

在 Developer PowerShell 中检查：

```powershell
.\build-scripts\check-multiplayer-build-env.ps1
```

核心构建与打包命令：

```powershell
msbuild -m `
  -p:Configuration=Release `
  -p:Platform=x64 `
  "-target:Cataclysm-vcpkg-static;JsonFormatter-vcpkg-static;zzip" `
  msvc-full-features\Cataclysm-vcpkg-static.sln

.\build-scripts\windist.ps1 -SDL3
```

正式基线以 `windows-2022` GitHub hosted runner 为准，Linux 上的 MinGW 交叉编译不能替代 MSVC 验证。

## 8. Fork remote

当前工作分支命名为 `multiplayer/main`，远端配置为：

```bash
git remote set-url origin https://github.com/wsdx233/Cataclysm-DDA-Multiplayer.git
git remote set-url --push origin https://github.com/wsdx233/Cataclysm-DDA-Multiplayer.git
git remote set-url upstream https://github.com/cleverraven/cataclysm-dda.git
git remote set-url --push upstream DISABLED
```

只允许向 `origin` 推送 fork 分支。不要恢复 `upstream` 的 push URL。

## 9. 当前验证状态

完整 hosted baseline run `29205262759`（run number `#5`，提交
`a39e06eb8620b377f515b6a8a7c8731b30543ebe`）于 2026-07-12 成功，三个最终产物均已下载到仓库外核对：

| 平台 | Artifact ID | GitHub artifact digest | 内部包 SHA-256 |
| --- | ---: | --- | --- |
| Linux curses x64 | `8263767548` | `4359fd56ddcb6948da9fffc38a5e37bf2a2457b748715aad30cdfebe5797235e` | `48f804153aa611350da3cbefaa474ce8e954b07a210184d480560180af3cde02` |
| Android arm64 | `8263779993` | `414789e43381505c3dbcca5efe5b5f4acca21ccdce6cf989fe7a29a4470cc14a` | `9b806ffab240500efd0776dcfab1954f9ce5ea611648ac36337c6415fecf0b99` |
| Windows client x64 | `8263854364` | `dc9ee8104ee91c6195ad6a566961a4a97010f6fa4b6527f6bca74476c0c91d97` | `09eab6ee7c4e74dcbfa0d201d9d46d438485706ba3fc34fb58df1ca8aa1f984c` |

- Linux tar 有 9,291 个 entries，archive、manifest SHA、`--version` 和动态库 smoke 均通过；输出为
  `-tiles, -sound`，因此仍只表示 curses 构建前身，不是 headless server。
- Android APK 的 ZIP 和内部 manifest SHA 通过；所有 6 个原生库均位于 `arm64-v8a/`，其中
  `libmain.so` 为 ELF64 AArch64。
  APK 声明 `android.permission.INTERNET`，且 `apksigner verify` 按预期报告 unsigned。
- Windows 内层 ZIP 的 `7z`/ZIP 完整性和 manifest SHA 通过，包含 9,048 个 files；
  `cataclysm-tiles.exe` 是 PE32+ x86-64 GUI executable。hosted 原生执行的 `windows-version.txt` 在 CRLF
  归一化后逐行精确包含 `+tiles, +sound`，不是仅凭工程配置推断 capability。
- translations、tileset、soundpack 和 shaders 的固定准备 jobs 同 run 全部成功。首个 Windows smoke 的
  GUI-process `$LASTEXITCODE` 假失败以及后续 CRLF regex 假失败均已被当前证据取代。

Transport spike run `29205262750`（同一提交）也全部成功：

| 平台 | Artifact ID | GitHub artifact digest |
| --- | ---: | --- |
| Linux GCC 13 + Clang 18 | `8263559175` | `f4d6e90086264b4989f0b6d21f4156caff15b7eb862cfa18dcbd025771e0cce4` |
| Android NDK arm64 | `8263560332` | `bbbba83a2053e4cc2a49ea43abba6b9306d70f19900553cf086bc1b659c9c197` |
| Windows MSVC | `8263560944` | `f9dd8750b4543f39d24e1efc1da327fc77afc71db7856f1423956d0996a04aa1` |

Linux 和 Windows 原生 artifacts 的 loopback 输出、所有内部 binary hash、Android ELF64 AArch64 header、
Asio pin 和 BSL-1.0 license 已核对。Android 是交叉编译门禁，不宣称在 hosted Android 设备上运行。该
spike 没有 TLS backend；发布安全限制见 ADR-0003。

2026-07-13 的当前未推送 Phase 1/2 source 已额外通过本地 GCC 13 release `cataclysm tests`、完整
`[multiplayer]`（30 cases / 996 assertions，3 个既有 `!mayfail` 对照）和 Android NDK arm64 debug APK
构建（`BUILD SUCCESSFUL in 6m 6s`）。这证明 Android source integration，但不能替代 hosted MSVC 或规范
artifact。`multiplayer-transport-spike.yml` 现还会在 Linux 构建真实 game/tests，并对真实
`cataclysm --server` 执行 auth/scene/command/resume/save process smoke。

本地生成物位于仓库默认的忽略目录中，不作为源码提交。规范产物和 hash 以 fork 上的 `multiplayer-baseline` workflow 为准。
