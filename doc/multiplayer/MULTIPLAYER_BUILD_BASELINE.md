# CDDA 联机 fork 原版构建基线

## 1. 目的

在修改运行模式、网络协议或玩家状态之前，先证明同一个提交能够稳定生成三类原版产物：

- Windows x64 MSVC 图形客户端，SDL3、tiles 和 sound 均启用。
- Android arm64 图形客户端 APK。
- Linux x64 curses 包，作为后续 headless server target 的构建前身。

Linux curses 包仍是无 SDL 的打包基线；同一 binary 已有显式 `--server` 运行路径，但在 hosted evidence、运维文档
和后续 release gate 完成前仍不能当作生产 dedicated-server 包发布。

初始上游基线固定为 `d84b90dd2aee090ca28c8dad5cdf1fab6dea151a`。

## 2. CI 入口

工作流位于 `.github/workflows/multiplayer-baseline.yml`，在以下情况运行：

- 手工触发 `workflow_dispatch`。
- 推送到 `multiplayer/main` 且构建相关文件发生变化。
- 面向 `multiplayer/main` 的 pull request 修改构建相关文件。

工作流不创建 GitHub Release，不需要 Android keystore，也不会使用正式发布凭据。每个产物旁边包含构建 manifest、源码提交和 SHA-256。
`workflow_dispatch` 始终选择 Linux、Windows、Android 全矩阵，供 Tier 3 milestone 使用。自动 push/PR run 先由
`Select affected platform packages` 比较 event base 与当前 commit，再只启用受影响的平台 package；若无法可靠
解析 base commit，则安全回退到全矩阵。

完整 `.po` 不在 Git 仓库中，而是在上游发布时从 Transifex 拉取。基线 workflow 不依赖 fork 私有的 Transifex token；它会校验并从固定的官方基线包提取已编译 `.mo`，供三端打包使用。

独立的 `.github/workflows/multiplayer-transport-spike.yml` 保留
`tools/multiplayer/transport_spike/` 的 Linux GCC/Clang、Windows MSVC loopback CTest 和 Android NDK arm64
交叉编译门禁；Linux job 还会构建真实 game/tests，运行完整 `[multiplayer]`、headless process smoke 和生产
`cataclysm --connect` PTY resume smoke。standalone spike 本身仍不构成生产 transport 或 TLS，生产边界位于
`src/multiplayer_*`。

### 2.1 何时运行平台构建

本文件保留三平台产物契约，但不要求每个 shared C++ 提交都完成整套 package。具体测试内容遵循重构计划
第 20.6 节的三层策略：

- Tier 1 日常默认使用 Linux native client/server、focused tests，并按风险增加完整 `[multiplayer]`、sanitizer
  与真实 PTY loopback。portable scheduler/game-rule/protocol routine 的 Linux 结果可作为日常功能证据。
- Tier 2 只为受影响的平台补定向证据：Windows-owned MSVC/project/batch/PowerShell/windist/Win32/SDL 变更跑
  Windows；Android Gradle/CMake/manifest/Java/JNI/ABI/resource/touch/lifecycle 变更跑 Android。shared C++ 本身
  不自动触发两端完整 package 要求。
- Tier 3 在 phase exit、release candidate、pinned toolchain/artifact contract 或 protocol compatibility milestone
  手工运行并记录必要的 Linux、Windows、Android 矩阵。

自动 baseline selector 的当前规则是：

- 普通 `src/multiplayer_*` gameplay/policy source 不触发 package baseline；它由 Linux production workflow 和
  Tier 1 本地验证负责。
- `android/**` 与 Android-owned build/runtime path 只选择 Android package 及其 translations/tileset/shaders 依赖。
- `msvc-full-features/**`、vcpkg triplet 和 Win32-owned source 只选择 Windows package 及其四类 resource 依赖。
- SDL/tiles/font/sound/ImGui 等共享 graphical UI path 选择 Windows 与 Android packages。
- Make/CMake、build scripts、data/lang/version、baseline workflow 和其他共享 build/artifact-contract path 选择全矩阵。
- 手工 `workflow_dispatch` 与无法解析 event base 的安全回退选择全矩阵。

changed-path selector 只是自动化优化，不替代工程判断。新增或重命名 platform-owned file 时，必须在同一改动中
同步维护 push/PR 的两份 path list 与 selector case mapping。若普通路径内部新增 platform conditional，路径匹配
无法自动识别；应显式运行受影响的 Tier 2 gate，并决定扩展 selector 或在 `STATUS.md` 记录继续手工触发的理由。

`multiplayer-transport-spike.yml` 的 Linux job 是 production tests/process-smoke 主门禁；Windows MSVC 与 Android
NDK jobs 明确只是 portable transport-only portability probes，不是完整 platform package 或 runtime gate。MinGW/NDK
cross-compile、MSVC loopback 或 compile-only APK 只证明各自边界，不能替代目标为 native package、resource 或
emulator/device lifecycle 时的完整平台 gate。

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
- `windows-help.txt`

CI 会执行打包目录中的 `cataclysm-tiles.exe --version` 与 `--help`，检查 network-client CLI，
并使用 `7z t` 检查压缩包。

### Android

- `cdda-android-arm64-baseline-unsigned.apk`
- `android-build-manifest.txt`
- `android-badging.txt`
- `android-permissions.txt`
- `android-resources.txt`
- 独立 compile-evidence artifact：`cdda-android-x86_64-debug.apk` 与
  `android-x86_64-build-manifest.txt`

该 APK 是 release 配置但未签名，适合验证构建内容，不用于安装或发布。CI 会检查 APK 完整性、包信息、
`arm64-v8a/libmain.so`、`android.permission.INTERNET`、multiplayer launcher resources，并确认没有混入
任何非 arm64 ABI。x86_64 debug APK 只用于 emulator/client source compile 门禁，必须只包含
`x86_64/libmain.so`，不属于 arm64 release 产物契约。

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

激活脚本也支持无 root 的本地工具链目录 `~/.local/toolchains/cdda-linux`。该目录存在时，可以使用其中的 GCC 13、Make、gettext、ncurses、zlib 和 bzip2 开发文件；正式 CI 仍使用 Ubuntu runner 的 Clang 18 系统包。用户级 prefix 必须同时包含或正确指向匹配的 ncursesw/tinfo runtime libraries，不能只解包 development linker script 和 static `libtinfo.a`。否则会把系统 shared ncurses 与 prefix static tinfo 混入同一 executable，并在 `initscr()` 发生 heap corruption。环境检查现会让 probe 同时引用 `initscr()` 与 `tparm()`，通过 `ldd` 要求动态 ncursesw/tinfo 来自同一 runtime root，并用 `nm` 拒绝静态 tinfo 实现混入 executable。

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

需要 Windows 证据时以 `windows-2022` GitHub hosted runner 为准，Linux 上的 MinGW 交叉编译不能替代 MSVC
验证。backend-neutral Tier 1 改动不因此自动要求 Windows package。

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

Phase 1 生产 source 后续已取得 hosted 绿色证据：baseline run `29219328448`（提交 `cd18703`）成功生成
Linux curses、Windows MSVC tiles+sound 和 Android arm64 artifacts；transport/protocol run `29219953446`
（提交 `bc7efe0`）成功通过 Linux GCC 13 production tests/process smoke、Linux GCC 13 与 Clang 18
standalone transport spike、Windows MSVC 和 Android NDK arm64 gates。该 transport run 的规范 artifacts 为：

| 平台 | Artifact ID | GitHub artifact digest |
| --- | ---: | --- |
| Linux GCC 13/Clang 18 + production game/process | `8268340815` | `ff04b8ecb4536e6e2bcc5a0aceaf7c04df8706c0aacf8dcda4f7a35998017d6e` |
| Windows MSVC | `8267835930` | `e509b6579b874ce57cfbabda488cbff24a48c2757a0ba19a6d3f286a75eaaf78` |
| Android NDK arm64 | `8267835710` | `1ea5da10ed8e951201d2cae95d58850b4edc982a69f52172ca2c0990ffb6ad5c` |

baseline run 的 artifact IDs/digests 与上述 transport 结果也同步记录在 `STATUS.md`。

2026-07-14 的当前 network-client 批次已通过本地 GCC 13 release `cataclysm tests`、完整 `[multiplayer]`
（53 cases；51 通过、2 个预期 `!mayfail` cases；3,066 assertions 中 3,063 通过、3 个预期对照），
focused server_lobby/dedicated_server/transport 17 cases / 1,686 assertions 和 client 12 cases / 711 assertions，
以及普通 release 的真实 `cataclysm --connect` PTY disconnect/resume/replay/ordered-release smoke；固定 backend
port 为 38265，command statuses 为 `0, 2, 0`，transcript 为 7,503 bytes。新增 transport regression 证明 logical
connection slot 会保留到 terminal event 被消费，connect/reset churn 不能挤掉 terminal control event；理论上
不可达的 control enqueue failure 会先写入独立、持久的 global fatal detail，再停止 transport。

最终 source 的 ASan/UBSan/LSan rebuild 与完整 `[multiplayer]` 同样为 53 cases（51 通过、2 个预期 cases；
3,066 assertions 中 3,063 通过、3 个预期对照），无 ASan/UBSan/LSan/stack-use-after-return finding。fresh-root
sanitizer PTY 使用 backend port 37263，客户端返回 0，得到相同 `0, 2, 0` statuses、精确 10 个 events 和
7,503-byte transcript，且无 sanitizer marker。第一次尝试只超过旧的 180 秒 server-startup allowance；成功重试
允许 sanitizer server 最多 600 秒启动，客户端交互 timeout 仍为 180 秒。

Android arm64 与 x86_64 debug APK 最终增量构建分别为
`BUILD SUCCESSFUL in 1m 21s` 与 `1m 19s`，SHA-256 分别为
`0f6d7324675c4291281d072ba631ab7a2477d9507033eefb3102b9fec6e6da6b` 和
`06e70f034bd9761aa98f6194798ffcc8706659ec387ebc590494340af1af66d1`；两份 APK 均核对 INTERNET
permission、单一目标 ABI `libmain.so` 和 multiplayer launcher resources。baseline workflow 现在还会在
Windows package 中检查 `--connect` CLI，在 Android APK 中检查 connect UI resources；transport workflow
会运行生产 network-client UI smoke。

### Phase 2 network-client batch hosted gate

source `6403a949fb537be14ec4d5757f295adbf5c5f99b` 的 hosted baseline run
[`29303564150`](https://github.com/wsdx233/Cataclysm-DDA-Multiplayer/actions/runs/29303564150) 为 terminal
`success`。平台 job durations 为 Linux `18m57s`、Android `39m04s`、Windows `32m34s`；规范 artifacts 为：

| Artifact | Artifact ID | GitHub artifact digest |
| --- | ---: | --- |
| Linux curses x64 | `8299663785` | `d9e228af65de0dcf52c4849a63a4de76267c8b1701de560518b13c01e4648069` |
| Windows x64 MSVC tiles+sound | `8299880246` | `1ca0a557fc27235bda25b2579542c4f058c8c554561612ff37c13018e9289b44` |
| Android arm64 release | `8299976619` | `d3d7382affbd7342951ca32957be2521165fbf2548f74e1d68d6ca22f86b4413` |
| Android x86_64 debug compile evidence | `8299977114` | `351094bbbd92664bb1245b586db978a257aed05daef6a51ae4316b8e39b3b16f` |
| Pinned tileset | `8299414855` | `6403077a11ae9696a41780f162553f980fcb594f1395b2d0f8daac9844eaadde` |
| Desktop shaders | `8299426143` | `098ede9628ca24e16ac8df6253ca8b88c26ce86e7d57fcd5048d274d674e92ee` |
| Pinned soundpack | `8299403377` | `37749d3ac9d82f8199a57f6ce603e0785bf3151062e95c9c22177a8921cacee4` |
| Compiled translations | `8299405280` | `b7ad074c2acb2ff9ae0fc9d7fae68cbb063082ec12332353c5f63f2b0156650b` |

同一 source 的 transport/protocol run
[`29303564152`](https://github.com/wsdx233/Cataclysm-DDA-Multiplayer/actions/runs/29303564152) 也为 terminal
`success`。job durations 为 Linux `36m05s`、Windows `58s`、Android `33s`：

| Artifact | Artifact ID | GitHub artifact digest |
| --- | ---: | --- |
| Linux GCC 13/Clang 18 + production game/process | `8299903967` | `0b38eaf73e9178895b8fe589573919582bf0144f6f7fa7346469966a14061013` |
| Windows MSVC | `8299413066` | `f203a9ab7fb6d59aa65fe5ea671ae0bb90621bc7c77e4d59c23d775da21ea430` |
| Android NDK arm64 | `8299407650` | `2c58a77c426dadf310948c945e64e25331dea2c7313ef2e62ab7effe41b37552` |

这两个 runs 关闭当前客户端批次的 hosted platform build gate。Android artifacts 仍只证明 package/resource/ABI
与 NDK compile；不能替代 emulator/真机 auth/render/wait/move、pause/resume 和 network reconnect smoke。

### Canonical multiplayer build ID 与本地 Android API 35 runtime

KVM 设备门禁首先暴露出旧 build identity contract 的跨后端缺陷：同一 source 的 Android SDL3 client 发送显示
版本派生的 `90e5fa3+SDL3`，headless server 发送 `90e5fa3`，因此握手被拒绝。commit
`e078eb6aef25a9cc72eea45793114c931a52b896`（`fix: use backend-neutral multiplayer build ids`）将 multiplayer
build ID 独立为 40 位小写 Git SHA，可选 `-dirty`；Make、CMake、Gradle、MSVC 使用同一值，显示 `VERSION` 和
backend/capability suffix 不参与协议。构建路径拒绝非法格式，runtime 在 ID 为空或不可取得时拒绝 multiplayer
startup；`--version` 会单独打印 `multiplayer build id`。baseline workflow 现在向 Linux、Windows、Android 显式
注入 `${github.sha}` 并在各 artifact 中验证该完整值。

本地环境 gate、Make valid/invalid/auto、direct CMake valid/no-rewrite/invalid、Gradle explicit/auto/invalid/no-Git
检查均通过；当时未在本地执行 MSVC batch，后续由下述 hosted `windows-2022` job 验证。clean Linux build 的
`./cataclysm --version` 显示 UI 版本 `e078eb6`、canonical ID
`e078eb6aef25a9cc72eea45793114c931a52b896` 和 `-tiles, -sound`。focused
`[multiplayer][protocol]` 为 8 cases / 240 assertions 全绿，完整 `[multiplayer]` 为 52 cases / 3,069 assertions，
仅保留三个既有 full-avatar-swap identity `!mayfail` 对照。

JDK 17 的 x86_64 Release clean build 使用：

```bash
source build-scripts/activate-multiplayer-build-env.sh
cd android
./gradlew clean \
  -Pj="$(nproc)" \
  -Pabi_arm_32=false -Pabi_arm_64=false \
  -Pabi_x86_32=false -Pabi_x86_64=true \
  -Plocalize=false assembleExperimentalRelease
```

结果为 `BUILD SUCCESSFUL in 54s`。unsigned APK SHA-256 是
`e3b308a16bb3b93192d9f107eeac7e1db303b407724ef5922596fb0d999d3287`；为 emulator 安装而使用 Android Debug
certificate 生成的 diagnostic signed copy SHA-256 是
`238f5343c5dc87d34ba062bdc573bf4464cae91c25ece08da1287a50eb3679b8`。debug signing 仅用于本地诊断，不是
production signing 或 hosted evidence。APK 的 `x86_64/libmain.so` 含有精确 canonical ID。

不修改系统权限即可通过 `sg kvm` 使用现有组权限：

```bash
source build-scripts/activate-multiplayer-build-env.sh
sg kvm -c "\"$ANDROID_HOME/emulator/emulator\" -accel-check"
```

输出报告 KVM version 12 usable；同一 API 35 x86_64 AVD cold boot log 为 `18.706s`。从 exported
`SplashScreen` 正常入口 cold launch，而不是 root/direct Activity launch，11,523 个 assets 在 90 秒内升级完成；
app 使用已保存 endpoint/token 与当次显式 LAN consent 进入 SDL client，全程没有 ANR。ASCIITiles remote scene 在
命令前已渲染，受控 wait 后仅发送 sequence 3/type 1，受控 right move 后仅发送 sequence 5/type 2，均 accepted；
move 前后截图有差异。

HOME 保留 native PID `4820`，但 generation 1 transport 断开；原 Activity 以 HOT `92ms` 恢复，Enter 使用同一
player/character identity 恢复 generation 2，随后 wait accepted。Android 内定向 `ss -K` 断开 generation 2；
airplane mode enable/disable 后 route 在 3 秒内恢复，Enter 再以同一 identity 恢复 generation 3，随后 wait
accepted。`Q` 断开 generation 3，server `SIGTERM` 完成 revision 9 save（220ms）并 shutdown。

本地 diagnostic package 缺少 `grayscale.frag.spv`，因此该 shader variant 被禁用，但 ASCIITiles scene 正常
渲染；hosted baseline 提供 shaders，所以这是本地诊断限制，不是 artifact contract 失败。测试结束后已恢复
`AUTO_KEYBOARD=true`、`show_ime_with_hard_keyboard=1`、`policy_control`/`hide_error_dialogs` null、airplane mode 0
和 AVD `hw.keyboard=no`，并停止 emulator。先前 `-accel off` 的 ANR 与 held-touch flood 只保留为不计入门禁的
诊断历史；本地 Android runtime gate 现为绿色。

同一 `e078eb6` source 的 hosted baseline run
[`29328086046`](https://github.com/wsdx233/Cataclysm-DDA-Multiplayer/actions/runs/29328086046) 为 terminal
`success`。Linux x64 curses、Windows x64 MSVC tiles+sound 与 Android arm64 release/x86_64 debug compile jobs
全部通过；三个主要 package jobs 分别约为 `3m10s`、`30m30s`、`40m49s`。关键 artifacts 为：

| Artifact | Artifact ID | GitHub artifact digest |
| --- | ---: | --- |
| Linux curses x64 | `8308833498` | `679fdda2d52fd71b9a63172d979494f5cb380998b6ee0a8fa385c372d0a68d9b` |
| Windows x64 MSVC tiles+sound | `8309536169` | `109c084bf44b8177ba9c5094402798d8c28af014fc149afc3d78c5c835da342b` |
| Android arm64 release | `8309741656` | `c6cc827c9f467a65e108efb19b43d50cb6a4649632328daec266396da4ea3697` |
| Android x86_64 debug compile evidence | `8309743121` | `16c24f0425f7fe2490b0c1ca90a9feee818d9823225137520b270b84549e6c12` |

artifact names 均带完整 `e078eb6aef25a9cc72eea45793114c931a52b896`。Linux `--version`、Android 两个
`libmain.so` 和 Windows artifact 的 `windows-version.txt` 均显示小写 canonical ID；Windows executable 同时报告
`+tiles, +sound`。需要精确区分：该 run 当时的 PowerShell gate 使用大小写不敏感的 `-notcontains`，所以不能把
run success 本身描述为“断言已区分大小写”；实际 artifact 文本是小写，后续 `86336ea` 才把 assertion 改为
`-cnotcontains`。

同一 source 的 transport/protocol run
[`29328086326`](https://github.com/wsdx233/Cataclysm-DDA-Multiplayer/actions/runs/29328086326) 也为 terminal
`success`：Linux GCC 13/Clang 18 job 通过 pinned FlatBuffers/Asio、生产 `[multiplayer]` tests、headless
command/resume smoke 和 local-input network-client UI resume smoke；Windows MSVC loopback 与 Android NDK arm64
compile jobs 也通过。关键 artifacts 为：

| Artifact | Artifact ID | GitHub artifact digest |
| --- | ---: | --- |
| Linux GCC 13/Clang 18 + production game/process | `8309668402` | `c020ceba6cae82c599599ba23a0e309af05b928af194cfa5cd0f998140522fff` |
| Windows MSVC | `8308785601` | `7ff2b126611a3f3325f2ffe97d5b7d6dd13e7d843e52b1ca81118d515995bc5c` |
| Android NDK arm64 | `8308782103` | `64f0e150407c73d046f5fec178fd1764c449570afe925d2d7d400b68e8cdd769` |

这两条 terminal-success runs 关闭 `e078eb6` 自身的 hosted gate；KVM Android lifecycle 证据仍精确属于该提交。

随后只读 build-path 审计发现三个需要单独修复的缺口：CMake `get_version` 依赖已有 `version.h`，可能跳过重算且
未把 configured override 传给实际生成脚本；Make 把 `git diff --quiet HEAD` 的任意非零值都当作 `-dirty`，会把
rc > 1 的 probe 错误降级成可用 identity；Windows workflow 的 ID assertion 不区分大小写。pushed commit
`86336ea847bea45f727fd97d74a811a32712518c`（`fix: harden multiplayer build id generation`）改为 always-run、
content-aware 的 CMake target 并传递 override，令 Make 对 rc > 1 fail closed，同时使用 PowerShell
`-cnotcontains`。

激活 pinned environment 后的本地复核结果：`make version MULTIPLAYER_BUILD_ID=$(git rev-parse HEAD)` 写入精确
clean `86336ea847bea45f727fd97d74a811a32712518c`；`GIT_INDEX_FILE=/ make version` 报告 dirty-state probe rc 128、
Make rc 2，且原 `version.h` SHA 不变。CMake auto build target 写入
`86336ea847bea45f727fd97d74a811a32712518c-dirty`，间隔一秒重复执行后 `version.h` mtime 不变；随后头文件已用
clean override 恢复。

最终 source `86336ea847bea45f727fd97d74a811a32712518c` 的 hosted baseline run
[`29330811779`](https://github.com/wsdx233/Cataclysm-DDA-Multiplayer/actions/runs/29330811779) 从
`2026-07-14T11:59:04Z` 运行至 `12:45:09Z`，7 个 jobs 全部 terminal `success`。Linux/Windows/Android package
以及四项 pinned resource artifacts 为：

| Artifact | Artifact ID | Size (bytes) | GitHub artifact digest |
| --- | ---: | ---: | --- |
| Android x86_64 debug compile evidence | `8311028317` | `286233515` | `1106f66e0d052e73cc9804f26fbc57f919f623b0fb793f152f204b345790a661` |
| Android arm64 release | `8311026327` | `180771669` | `416606fdd8cb75f0573aa6e0ec1825d258696cfbb95fce0a0d7018b1be6cf7f8` |
| Windows x64 MSVC tiles+sound | `8310637856` | `314192240` | `3584f373bdd0087664126c47cb8baa98b6fbaab8112cafd9bbd949f2a96221a3` |
| Linux curses x64 | `8309916226` | `183112210` | `9d012e8ca91b5142f44a9c514fe9a27849d64c1710b7ab8576b08210a160572a` |
| Desktop shaders | `8309904857` | `28669` | `9097d36d426b1a0059a47e822cf539509b91bb63135b418a6b5a408038f4bf6a` |
| Pinned default tileset | `8309890914` | `4733394` | `bb638007a3f6d01821ccca8a79ac02e8de1d84124e9662d46429dd07797b373b` |
| Compiled translations | `8309873347` | `85605345` | `aa132c98c3cfae2b40e9d958c11ec8e1ed82a7c1a2f014a73bbafb511c5cbd12` |
| Pinned soundpack | `8309872172` | `137709159` | `2c75644dc5b984aac604f0de508045d3f555ec820b2005a56ec9d39ccc0e4504` |

Windows native `--version` smoke 通过新的大小写敏感 `-cnotcontains` assertion；Linux Make、Windows MSVC 与
Android Gradle package paths 的 artifact checks 均确认精确小写 full SHA。顶层 `src/CMakeLists.txt` 的
always-run/override path 不由这些 hosted package jobs 直接调用，其直接证据是上文记录的本地 Ninja/Unix
Makefiles 回归。

同一 source 的 transport/protocol run
[`29330811746`](https://github.com/wsdx233/Cataclysm-DDA-Multiplayer/actions/runs/29330811746) 从
`2026-07-14T11:59:04Z` 运行至 `12:37:29Z`，Linux GCC 13/Clang 18、Windows MSVC 与 Android NDK arm64 三个
jobs 全部 terminal `success`：

| Artifact | Artifact ID | Size (bytes) | GitHub artifact digest |
| --- | ---: | ---: | --- |
| Linux GCC 13/Clang 18 + production game/process | `8310820503` | `147781` | `8422a7f7bcfd4ddea02145e287f267bfa1ba44f7d365d99d795c664f9ec0b364` |
| Windows MSVC | `8309890710` | `54055` | `c03af2a211ab5221cf38ab4271a774e67e8464da343fa1e636284858e4a67ad7` |
| Android NDK arm64 | `8309880601` | `1705305` | `c10815e1a2fbfda78531e453c7f23a50365b7aef80d3b498889d2d0e46f2c074` |

该 run 重跑 pinned schema/Asio、GCC/Clang/MSVC loopback、生产 `[multiplayer]` tests、真实 headless
command/resume smoke、local-input network-client UI resume smoke 和 Android NDK compile。两条最终 runs 关闭
canonical build-ID hardening 的 hosted gate；结合精确归属于 `e078eb6` 的 Android KVM lifecycle，Phase 2 于
2026-07-14 正式关闭。

### Phase 3 scheduler source 与 Windows validator 定向证据

首个纯 scheduler policy 的 source commit 为 `45be077ac2d0d042190e54d1bdb77d48676b67e6`。它的
transport/protocol run
[`29340055386`](https://github.com/wsdx233/Cataclysm-DDA-Multiplayer/actions/runs/29340055386) 已 terminal
`success`：Linux GCC 13/Clang 18 job 通过 production tests 与 process smokes，Windows x64 MSVC loopback 和
Android NDK arm64 compile jobs 也成功。

同一 source 的 baseline run
[`29340056602`](https://github.com/wsdx233/Cataclysm-DDA-Multiplayer/actions/runs/29340056602) 为 terminal
`failure`，但 Linux curses、Android package、translations、tileset、shaders 和 soundpack jobs 均成功。唯一失败
是 Windows x64 MSVC tiles+sound package job
[`87109602068`](https://github.com/wsdx233/Cataclysm-DDA-Multiplayer/actions/runs/29340056602/job/87109602068)
的 `Build package` step：`msvc-full-features/prebuild.cmd` 的手写 validator 误拒合法 40 位小写 canonical SHA。
这不是 scheduler source 的 MSVC compile failure；同一提交的 transport Windows MSVC job 已绿色。

commit `c9b28086e973fa497d5bd9f9a37e64a9ac22e464` 让 `prebuild.cmd` 从环境读取 ID，并用 anchored、
case-sensitive PowerShell regex `\A[0-9a-f]{40}(?:-dirty)?\z` 校验；baseline workflow 还会在 MSBuild 前显式
运行 prebuild，并在 prebuild 或 MSBuild 返回非零时立即失败。baseline run
[`29344937410`](https://github.com/wsdx233/Cataclysm-DDA-Multiplayer/actions/runs/29344937410) 的 attempt 1 已让
Windows x64 MSVC tiles+sound package job
[`87126475702`](https://github.com/wsdx233/Cataclysm-DDA-Multiplayer/actions/runs/29344937410/job/87126475702)
完整 terminal `success`，包括 prebuild、MSBuild、windist、native `--version`/`--help` 和 package smoke；这关闭
该 Windows-specific validator 修复的 Tier 2 gate。

同一 attempt 的 Linux package 与四个 resource jobs 也成功。Android job
[`87126475664`](https://github.com/wsdx233/Cataclysm-DDA-Multiplayer/actions/runs/29344937410/job/87126475664)
已完成 arm64 release build，但随后的 x86_64 debug build 因 hosted runner `No space left on device` 失败；attempt 2
的 Android-only job
[`87137475194`](https://github.com/wsdx233/Cataclysm-DDA-Multiplayer/actions/runs/29344937410/job/87137475194)
随后按新的分层验证策略由用户取消。Android 没有被 `c9b2808` 修改，且 `45be077` 已有成功的 Android package 与 NDK
compile evidence，因此这两次 Android 结果既不是 scheduler/code failure，也不阻塞 Linux-first Phase 3 工作。
run `29344937410` 不能标成 terminal-green 全平台矩阵；它的可用结论是 Windows Tier 2 成功、Android attempt 1
runner-capacity failure、attempt 2 policy cancellation。Phase 3 退出时仍须按 Tier 3 为同一候选 source 记录必要
平台矩阵；当前 `players.max` 必须保持 `1`。

本地生成物位于仓库默认的忽略目录中，不作为源码提交。规范产物和 hash 以 fork 上的 `multiplayer-baseline` workflow 为准。
