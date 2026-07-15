# CDDA 联机 fork 原版构建基线

## 1. 目的

Phase 0 已先证明同一个提交能够稳定生成三类原版产物，并把它们保留为长期 package/artifact 契约：

- Windows x64 MSVC 图形客户端，SDL3、tiles 和 sound 均启用。
- Android arm64 图形客户端 APK。
- Linux x64 curses 包，作为后续 headless server target 的构建前身。

这项历史三端基线不是每次 shared-code 修改的验收要求。Linux curses 包仍是无 SDL 的打包基线；同一 binary 已有
显式 `--server` 运行路径，但在 hosted evidence、运维文档和后续 release gate 完成前仍不能当作生产
dedicated-server 包发布。

初始上游基线固定为 `d84b90dd2aee090ca28c8dad5cdf1fab6dea151a`。

## 2. CI 入口

工作流位于 `.github/workflows/multiplayer-baseline.yml`，在以下情况运行：

- 手工触发 `workflow_dispatch`。
- 推送到 `multiplayer/main` 且构建相关文件发生变化。
- 面向 `multiplayer/main` 的 pull request 修改构建相关文件。

工作流不创建 GitHub Release，不需要 Android keystore，也不会使用正式发布凭据。每个产物旁边包含构建 manifest、源码提交和 SHA-256。
`workflow_dispatch` 默认 `target=linux`。手工运行者只在已经完成的 platform-owned/public-boundary/release 批次
需要对应 package 时选择 `windows`、`android` 或 `all`；phase exit 本身不要求选择 `all`，只有明确需要 all-platform
package 的 release/artifact/兼容性里程碑才使用它。自动 push/PR run 先由
`Select affected platform packages` 比较 event base 与当前 commit，再只启用受影响的平台 package；若无法可靠
解析 base 或 diff，则快速失败并要求用明确的 manual target 重跑，不再静默消耗全矩阵。

完整 `.po` 不在 Git 仓库中，而是在上游发布时从 Transifex 拉取。基线 workflow 不依赖 fork 私有的 Transifex token；它会校验并从固定的官方基线包提取已编译 `.mo`，供三端打包使用。

独立的 `.github/workflows/multiplayer-transport-spike.yml` 保留
`tools/multiplayer/transport_spike/` 的 Linux GCC/Clang、Windows MSVC loopback CTest 和 Android NDK arm64
交叉编译门禁；Linux job 还会构建真实 game/tests，运行完整 `[multiplayer]`、headless process smoke 和生产
`cataclysm --connect` PTY resume smoke。手工运行默认 `target=linux`；自动普通多人源码改动只跑 Linux。Windows/
Android job 只在 standalone spike/workflow 自身变化或显式 target 时运行。standalone spike 本身仍不构成生产
transport 或 TLS，生产边界位于 `src/multiplayer_*`。

### 2.1 验证节奏与平台构建

本文件保留三平台产物契约，但不要求每个 shared C++ 提交都完成整套 package。具体测试内容遵循重构计划
第 20.6 节的三层策略：

- Tier 1 编辑循环只做 Linux incremental build 和 changed-area focused tests。一个 coherent slice 准备收口时，
  无 production caller 的 internal/test-only contract 按 invariant/ownership 风险选择完整 `[multiplayer]` 或
  sanitizer；只有 changed route 能被 production process 实际调用时才增加对应 Linux gate：client-only 使用 native
  client，server-only 使用 headless process，end-to-end 才使用两端 PTY/loopback。Linux curses client 是非图形共享
  行为的默认代表，renderer/tiles 行为才使用 Linux SDL client。
- Tier 2 只在关键 platform-owned 或跨平台公共边界批次完成后运行一次定向证据：Windows-owned MSVC/project/
  batch/PowerShell/windist/Win32/SDL 批次跑 Windows；Android Gradle/CMake/manifest/Java/JNI/ABI/resource/touch/
  lifecycle 批次跑 Android；schema/public header/ABI boundary 只跑受影响消费者中能实际编译 changed production
  source 的最小 MSVC 和/或 NDK gate。shared internal `.cpp` 本身不自动触发两端 package 要求，也不为每个中间提交
  重复平台门禁。
- Tier 3 是 phase/release milestone evidence review。phase exit 先核对产品声明、当前候选新证据和最近兼容平台证据，
  不自动运行全平台；当前 Linux 候选仍运行阶段要求的 headless/native-client、integration 和 sanitizer 主门禁。
  未变化的平台边界可以引用最近 evidence commit，并记录到当前候选的显式 diff audit 与兼容理由；必须同时注明
  当前候选未在该平台编译或运行。release candidate、pinned toolchain/artifact/signing contract、显式跨版本兼容或
  新的平台行为声明仍必须取得所有受影响平台的新证据。

自动 baseline selector 的当前规则是：

- 普通 `src/multiplayer_*` gameplay/policy source 不触发 package baseline；它由 Linux production workflow 和
  Tier 1 本地验证负责。
- `src/multiplayer_transport.cpp`、`src/multiplayer_crypto.cpp`、server config/log 和混合的 `src/main.cpp` 视为
  routine shared 或需要人工判断的 generic path，不再仅凭文件名触发 Windows/Android package；若 diff 修改其中
  的 platform conditional，提交者必须显式选择受影响的 Tier 2 target。
- 当前 public `multiplayer_transport.h`/`multiplayer_crypto.h` 改动仍选择 Windows 与 Android package，以确保实际
  production source 被对应编译器编译；后续建立轻量 production portability target 后可替换这项较重门禁。
- protocol implementation/header、generated FlatBuffers header 或 `.fbs` schema 变化同样选择 Windows 与 Android
  package；Linux transport job先校验 generated header并构建 production tests，三端 package 实际编译生成的
  production protocol source。轻量 target 建立前，这些 package job 只是 Tier 2 actual-source compile fallback；
  package/resource/runtime 行为不是自动扩大的验收目标。纯 protocol README 不触发该门禁。
- `android/**` 与 Android-owned build/runtime path 只选择 Android package 及其 translations/tileset/shaders 依赖。
- `msvc-full-features/**`、vcpkg triplet、Windows PowerShell/MSVC/windist script 和 Win32-owned source 只选择
  Windows package 及其四类 resource 依赖。
- SDL/tiles/font/sound/ImGui 和 client UI 等共享 graphical/platform path 选择 Windows 与 Android packages；
  filesystem/mmap/path/locale adapter 选择全矩阵。
- Linux/Android 环境脚本只选择 Linux 与 Android，明确 Linux-only script 只选择 Linux；`Makefile` 只选择实际使用
  它的 Linux package。root `CMakeLists.txt`/`src/version.cmake` 不再触发无关 package，而由 Linux production
  workflow 的定向 root-CMake configure + `get_version` target 验证。其余共享 build scripts、data/lang/version、
  baseline workflow 和真正的 artifact-contract path 才选择相应矩阵。
- 手工 baseline 默认 `target=linux`；`all` 只用于明确的 all-platform package/release/artifact/兼容性目标。自动
  selector 无法解析 event base/diff 时失败并要求显式重跑，不能把分类失败转换成昂贵的隐式全矩阵。

changed-path selector 只是自动化优化，不替代工程判断。新增或重命名 platform-owned file 时，必须在同一改动中
同步维护 push/PR 的两份 path list 与 selector case mapping。若普通路径内部新增 platform conditional，路径匹配
无法自动识别；修改已有 platform conditional/branch 也同样如此。应显式运行受影响的 Tier 2 gate，并决定扩展
selector 或在 `STATUS.md` 记录继续手工触发的理由。

CI 成本优化 backlog：generic `build-scripts/*` 与 `data/*|lang/*` 目前仍可能选择全矩阵。不要在没有 package input
依赖清单和 selector 回归断言时直接缩窄；后续以独立 CI-contract 批次审计真实消费者，把仅影响 Linux 或单个平台的
路径拆出。该优化不阻塞 Phase 3 backend-neutral rule/session 工作。

自动 selector 若因中间 push 启动平台 package，只表示该提交命中了路径规则，不会把每个后续中间提交变成必须
重复的平台门禁。关键批次冻结后，应使用最后一次修改该平台/公共边界的候选证据；若最终候选只追加了可审计的
内部改动，可以按第 20.6 节记录 evidence commit 与 diff audit，而不能声称最终候选已在该平台编译。

当前 workflow 不会自动把多个 push 合并成一个 coherent batch：每个匹配的 push/PR 都可能启动 package，concurrency
只会取消仍在运行的旧 job。要实际避免重复平台消耗，应把中间 platform/public-boundary commits 保留在本地或 topic
branch，并尽量只向受监控分支推送冻结的验收候选。后续应以轻量 production portability target 或显式 batch trigger
替换这项操作纪律；在那之前，自动启动的中间 run 不改变第 20.6 节的规范门禁，但仍会实际消耗 CI。

`multiplayer-transport-spike.yml` 的 Linux job 是 production tests/process-smoke 主门禁；Windows MSVC 与 Android
NDK jobs 明确只编译 `tools/multiplayer/transport_spike/`，不编译 changed production `src/multiplayer_*`。因此它们
只证明 standalone spike、Asio pin 和 compiler/toolchain contract，不是普通 scheduler/adapter/transport 实现的
平台证据，也不是完整 platform package/runtime gate。MinGW/NDK cross-compile、MSVC loopback 或 compile-only APK
只证明各自边界，不能替代目标为 native package、resource 或 emulator/device lifecycle 时的完整平台 gate。

2026-07-14/15 的后续 selector source 已通过本地静态/动态门禁：两个 workflow 的 actionlint、YAML parse 和全部
Bash-compatible run block `bash -n` 通过；transport manual 四种 target、routine scheduler diff 的 Linux-only、
transport-workflow diff 的三端选择、root-CMake output 和两个 selector 的 unresolved-base fail-fast 均有动态断言。
本地 GCC 13 root CMake configure + `get_version` target 也通过。静态 mapping 断言确认 protocol/schema/generated
header 走 W/A actual-source package、Makefile 只走 Linux，并移除纯 README/Windows-only props 的无效 Linux trigger。
commit `804101995c175057856529be24439b0d7e87a49a` 的首次 hosted transport 与 baseline runs 已全部 terminal
`success`，关闭 workflow 自身的 cross-platform CI boundary；详细 jobs/artifacts 见第 9 节末尾。之后普通
production source push 以 Linux-only 为默认，除非命中明确 Tier 2/3 trigger。

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
Android NDK arm64 compile jobs 也成功。后两项只编译 standalone transport spike，不编译 scheduler production
source，因此不能作为该 scheduler 的 MSVC/NDK source evidence。

同一 source 的 baseline run
[`29340056602`](https://github.com/wsdx233/Cataclysm-DDA-Multiplayer/actions/runs/29340056602) 为 terminal
`failure`，但 Linux curses、Android package、translations、tileset、shaders 和 soundpack jobs 均成功。唯一失败
是 Windows x64 MSVC tiles+sound package job
[`87109602068`](https://github.com/wsdx233/Cataclysm-DDA-Multiplayer/actions/runs/29340056602/job/87109602068)
的 `Build package` step：`msvc-full-features/prebuild.cmd` 的手写 validator 误拒合法 40 位小写 canonical SHA。
这不是 scheduler source 的 MSVC compile failure；Linux production tests 与 Android APK build 已实际覆盖 scheduler
source，而该 run 没有取得 Windows production compile 证据。Windows evidence 由下述修复提交的完整 MSVC package
job补齐，不能用 standalone transport probe 替代。

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
随后按新的分层验证策略由用户取消。Android 没有被 `c9b2808` 修改，且 `45be077` 已有实际编译 production source
的成功 Android package；同批 NDK job 只属于 standalone transport toolchain evidence。因此这两次 Android 结果
既不是 scheduler/code failure，也不阻塞 Linux-first Phase 3 工作。
run `29344937410` 不能标成 terminal-green 全平台矩阵；它的可用结论是 Windows Tier 2 成功、Android attempt 1
runner-capacity failure、attempt 2 policy cancellation。Phase 3 退出时必须复核当前候选的新证据与未变化平台的
最近兼容 evidence/diff audit，但不因 phase exit 自动要求同一候选重跑全平台；任何引用都不得声称未运行平台已经
编译当前候选。当前 `players.max` 必须保持 `1`。

### 分层 package selector hosted gate

commit `377feba3b22f3ddafbaf259f26c4871791e9fbc6` 引入 Tier 1/2/3 文档与 package selector，commit
`6155cc9603f942ae9c4c86d69dc1d66a5fdc6a94` 把 selector 改为 shallow checkout + 按需抓取 event base；最终
workflow source `9a561f6172d9042c1c424f16d923448e0de9152a` 又补齐已知 platform-owned production source、单平台
`workflow_dispatch target` 与 Windows-only build-script 分类。

最终 baseline run
[`29353291753`](https://github.com/wsdx233/Cataclysm-DDA-Multiplayer/actions/runs/29353291753) 从
`2026-07-14T17:19:37Z` 运行至 `18:04:23Z`，8 个 jobs 全部 terminal `success`。selector job
[`87154675790`](https://github.com/wsdx233/Cataclysm-DDA-Multiplayer/actions/runs/29353291753/job/87154675790)
只用 9 秒解析 `6155cc9..9a561f6`，因 workflow 自身变化按规则输出 Linux/Windows/Android 全 true；四项 resource、
Linux package、Windows MSVC package 与 Android arm64 release/x86_64 compile 都成功。平台 artifact 为：

| Artifact | Artifact ID | Size (bytes) | GitHub artifact digest |
| --- | ---: | ---: | --- |
| Linux curses x64 | `8319128325` | `183175299` | `a5ce693cfbcc5eb389f45319577316612bd3d24a416aa7f6875c9e67ba25acb3` |
| Windows x64 MSVC tiles+sound | `8319911321` | `314221912` | `d9b835b0186f3fef76cf6e3946e70675e0e8a9ae04fb308ad5028b296e289d07` |
| Android arm64 release | `8320275610` | `180793685` | `48fd29f85c02cce26a408a18f8352dd35748c8a71641fd5190f95af42ecf7c25` |
| Android x86_64 debug compile evidence | `8320277669` | `286299819` | `c5ecdbb4c2b54b801d4bc02444c7a8858826029c69562ff4798b743bb48b1e8f` |

同批 transport run
[`29351194814`](https://github.com/wsdx233/Cataclysm-DDA-Multiplayer/actions/runs/29351194814) 也 terminal
`success`：Linux production tests/process smokes、Windows MSVC portability probe 与 Android NDK portability
probe 全绿。它们的 artifact IDs 分别为 `8319340742`、`8318224196`、`8318217657`。较早 baseline run
`29351421997` 因最终 selector 修正推送而被 concurrency 取消；其 selector/resources/Linux 已成功，Windows/Android
由上述 terminal-success run 取代，不能把该 cancellation 记为代码失败。

这组 evidence 关闭 selector/workflow 自身的关键 CI 里程碑，不重新建立“每个 shared C++ 提交必须全平台 package”
的旧约束。Android-only、Windows-only、shared graphical/platform、all-platform adapter、Windows-only build script
和 `workflow_dispatch all|linux|windows|android` 分类已由本地动态断言覆盖；未来实际 Tier 2 仍只运行受影响 target。

### Linux-first 最终 selector 与 phase-adapter 批次 hosted gate

最终 source `804101995c175057856529be24439b0d7e87a49a` 同时包含 test-owned phase adapter 和最终
Linux-first workflow/selector 修正。transport run
[`29377566098`](https://github.com/wsdx233/Cataclysm-DDA-Multiplayer/actions/runs/29377566098) 已 terminal
`success`：selector job `87234082789`、Windows isolated MSVC probe `87234108316`、Linux production job
`87234108318` 与 Android isolated NDK probe `87234108325` 全部成功。Linux job 实际运行 root-CMake
configure/`get_version`、FlatBuffers/Asio gate、GCC production build、完整 `[multiplayer]`、headless
command/resume smoke 和 native client UI reconnect/resume smoke。transport artifacts 为：

| Artifact | Artifact ID | GitHub UI size | GitHub artifact digest |
| --- | ---: | ---: | --- |
| Linux production/provenance | `8329082394` | `144 KB` | `555ea053beb0825a304ea210dda4be7ecdf2bbe56464b6c3ddece6cc03e9a845` |
| Windows isolated spike | `8328455030` | `52.8 KB` | `a14db45adb476e37ddfdbcdcc55fdfdcf714180c14bf4687bde92915a253f579` |
| Android isolated spike | `8328453980` | `1.63 MB` | `b238e479b7396c70316da6f0e72f1075cc996670df37e9f17fd6bf200ebab1d3` |

同一 source 的 baseline run
[`29377566114`](https://github.com/wsdx233/Cataclysm-DDA-Multiplayer/actions/runs/29377566114) 也 terminal
`success`。selector job `87234082868`、shaders `87234105207`、soundpack `87234105233`、translations
`87234105243`、tileset `87234105274`、Linux package `87234154681`、Windows package `87234324264` 与 Android
package `87234324282` 共 8 个 jobs 全绿。平台 artifacts 为：

| Artifact | Artifact ID | GitHub UI size | GitHub artifact digest |
| --- | ---: | ---: | --- |
| Linux curses x64 | `8328662814` | `175 MB` | `c893cf53ffeb6301223fe8c1a52c2a1b09ed7b3cb797df69cf6b59ba4c440401` |
| Windows x64 MSVC tiles+sound | `8329011211` | `300 MB` | `ba70df76e9b687b6e5a673b83d5acfaee53a0535140c9ebc773cd278f542fba3` |
| Android arm64 release | `8329209090` | `172 MB` | `0751d131514060f5769bb9356ce6eb7f7a56a489f5a179bec4fff48e42c9f704` |
| Android x86_64 debug compile evidence | `8329210402` | `273 MB` | `13856e99b2853b8c23ac3dd3b9366b55f7cb1e70902a42bfdf8933c289437055` |

这两条 runs 关闭本次 workflow/selector CI-contract milestone。phase-adapter gameplay slice 的 acceptance 仍是
Linux Tier 1；Windows/Android transport artifacts 只证明 isolated spike/toolchain，而 package matrix 只证明该
提交的 package/artifact workflow 成功。除非对应 job 实际编译或运行 changed production source，不得把这些结果
外推为 adapter 的平台 portability 或 production routing evidence。后续普通 backend-neutral source 提交继续使用
Linux incremental/focused loop；slice 收口按风险增加 Linux 门禁，完成关键平台/公共边界批次或提出需要新证据的
release/platform 行为声明时再升级。

### Baseline 手工入口 Linux 默认值

workflow source `25347cc3d5538657e67790e3d7a83aaae25b796c` 把 baseline `workflow_dispatch` 的默认 target 与表达式
fallback 从 `all` 改为 `linux`，并把 `all` 放到显式选择末尾。automatic changed-path selector、package job、artifact
内容和 pinned toolchain 均未改变。

本地用 actionlint `1.7.12`（官方 release archive checksum 已校验）检查 workflow，通过 PyYAML parse 和 selector
run-block `bash -n`；动态执行 `linux|windows|android|all` 四种 manual target，分别得到精确单平台或三平台 true
输出。该验证只关闭 dispatch/selector control contract，不构成 Linux/Windows/Android package 或 runtime 证据；
hosted run 只有 terminal 后才可追加记录。当前 automatic selector 仍把 baseline workflow 自身的任何修改归为
all-platform CI-contract，因此该 source 推送后可能触发一次全 package matrix；后续应把纯 dispatch/selector 静态
控制变更拆到轻量 workflow-control gate，避免再为这类改动构建产物，同时保留真正 package-job 修改的全矩阵门禁。

### Authoritative session directory / protocol minor 1 hosted gate

source `eb990c4ad9975915336f3acd65431b47d123e842` 的 Linux production run
[`29385561675`](https://github.com/wsdx233/Cataclysm-DDA-Multiplayer/actions/runs/29385561675) 与 baseline run
[`29385561653`](https://github.com/wsdx233/Cataclysm-DDA-Multiplayer/actions/runs/29385561653) 均为 terminal
`success`。这一批修改 `ResumeRequest` schema/generated header 和 protocol minor，因此按 Tier 2 public
boundary 增加实际编译 changed production source 的 MSVC/Android evidence；它不是 phase exit、release 或跨
minor 互通承诺。

transport selector job `87257955192` 只启动 Linux production，standalone Windows/Android transport probes 均
`skipped`。Primary Linux job
[`87257984217`](https://github.com/wsdx233/Cataclysm-DDA-Multiplayer/actions/runs/29385561675/job/87257984217)
为 terminal `success`；pinned FlatBuffers generation、GCC 13/Clang 18 transport、production GCC build/完整
`[multiplayer]`、headless command/resume 与 native-client UI resume smokes 全部通过。

baseline selector job `87257955175` 精确跳过 Linux curses package，只启动了当前轻量 production
portability target 建立前的 Windows/Android actual-source package fallback。平台 artifacts 为：

| Artifact | Artifact ID | Size (bytes) | GitHub artifact digest |
| --- | ---: | ---: | --- |
| Linux production/provenance | `8331837770` | `147797` | `c52b3d14df1d21e96f9ca945c29cfabd00f75f938cad258608f08a2391e1f218` |
| Windows x64 MSVC tiles+sound | `8331645754` | `314281245` | `1fc1f71158cc8c46d3ad2239db06779fa6f5c7ba031b2b412237a6269a01d65e` |
| Android arm64 release | `8331840045` | `180840533` | `883fc5a3c91fcc40deb0d3b0325e461a9b213ebf37abf8d8aa10a11e8b14b751` |
| Android x86_64 debug compile evidence | `8331840920` | `286403755` | `503ab5e251cd65499f19ed008c9e0848a6a9def1e4eb4585379edad4c461a758` |

Windows job
[`87258227728`](https://github.com/wsdx233/Cataclysm-DDA-Multiplayer/actions/runs/29385561653/job/87258227728)
和 Android job
[`87258227735`](https://github.com/wsdx233/Cataclysm-DDA-Multiplayer/actions/runs/29385561653/job/87258227735)
均为 terminal `success`，并通过各自 package/provenance smoke。这些结果关闭本次 public compile boundary，但
不增加 Windows UI 或 Android emulator/device lifecycle 运行结论，也不要求后续 internal session/scheduler
`.cpp` 改动日常重跑 package matrix。

本地生成物位于仓库默认的忽略目录中，不作为源码提交。规范产物和 hash 以 fork 上的 `multiplayer-baseline` workflow 为准。
