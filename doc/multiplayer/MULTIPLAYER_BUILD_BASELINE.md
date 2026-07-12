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

该 APK 是 release 配置但未签名，适合验证构建内容，不用于安装或发布。CI 会检查 APK 完整性、包信息、`arm64-v8a/libmain.so`，并确认没有混入 32 位 ARM 库。

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

2026-07-12 在基线提交 `d84b90d` 上完成：

- Linux x64 curses `bindist` 构建、tar 完整性、`cataclysm --version` 和 `ldd` 检查通过。
- Android arm64 debug APK 构建、ZIP 完整性、ABI、badging 和 debug v2 签名检查通过。
- Android arm64 unsigned release APK 构建、ZIP 完整性、ABI、badging 和未签名状态检查通过。
- 本机不是 Windows，Windows 结果必须以 `windows-2022` hosted runner 为准。

2026-07-12 的首个 hosted baseline run `29177657248`（提交 `0955d865ea170c26511a789ce9d37334ac530953`）中：

- 固定 translations、tileset、soundpack 和 shaders 的准备作业成功。
- Linux x64 curses job 成功并上传 artifact `cdda-linux-curses-x64-baseline`。
- Android arm64 job 成功并上传 artifact `cdda-android-arm64-baseline`。
- Windows MSVC job 以 `0 Error(s)` 完成编译并由 `windist.ps1 -SDL3` 完成打包；随后 version smoke 因 PowerShell 直接调用 GUI subsystem exe 后 `$LASTEXITCODE` 为 null 被误判失败，未执行 artifact upload。
- workflow 已改为通过 `Start-Process -Wait -PassThru` 取得显式进程退出码并验证版本输出；该修复通过 actionlint 1.7.12，但尚未在 hosted runner 重跑。

因此 Linux/Android hosted artifacts 已有成功证据，Windows compile/package 已有成功证据，但完整三平台 artifact 基线仍未关闭。必须取得新 run 的 Windows version smoke、ZIP 完整性、provenance 和 artifact upload 成功结果后，才能把 Windows 和完整基线标为通过。

本地生成物位于仓库默认的忽略目录中，不作为源码提交。规范产物和 hash 以 fork 上的 `multiplayer-baseline` workflow 为准。
