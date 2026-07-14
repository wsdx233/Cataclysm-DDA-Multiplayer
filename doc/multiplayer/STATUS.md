# CDDA 多人 fork 当前状态

- 更新日期：2026-07-14
- 分支：`multiplayer/main`
- 当前阶段：**Phase 2，单远程玩家垂直切片**
- 上游基线：`d84b90dd2aee090ca28c8dad5cdf1fab6dea151a`
- 当前已推送 source 提交：`6403a949fb537be14ec4d5757f295adbf5c5f99b`（Phase 2 production network-client batch）

## 当前结论

Phase 0 已关闭，ADR-0001 至 ADR-0009 继续有效。Phase 1 的本地门禁和 hosted 平台门禁都已取得绿色证据：生产 dedicated runtime、Asio transport、FlatBuffers 协议、严格握手/content manifest、token auth、canonical server-owned avatar、结构化日志、signal save/shutdown 和真实进程 command/resume smoke 均已验证。

Phase 2 的服务器侧单远程玩家纵向切片保持完成。本批又实现了可复用的生产客户端 transport/state machine、桌面/Android connection UI、本地 input → semantic wait/move、visibility-filtered scene → 本地 curses/tiles renderer、heartbeat/manual reconnect、断线 resume、exactly-once replay 和按 simulation FIFO 完成的 typed clean session release。最终 source 的普通 release 与 ASan/UBSan binary 都已通过真实 Linux PTY auth、scene、wait、强制断线、resume、未确认命令重放、move 和 clean quit。

本批提交 `6403a949fb537be14ec4d5757f295adbf5c5f99b` 的 hosted platform build gate 已关闭：baseline run
`29303564150` 和 transport/protocol run `29303564152` 均为 terminal `success`，覆盖 Linux curses、Windows x64
MSVC tiles+sound、Android arm64 release/x86_64 debug、生产 Linux process/client smoke、standalone GCC/Clang/MSVC
transport 和 Android NDK arm64。

**Phase 2 仍不标记关闭。** Android 尚没有 emulator/真机 auth/render/wait/move、pause/resume 或 network
disconnect/reconnect 证据；hosted APK/NDK compile 不是运行时 smoke。服务器仍只允许一个 remote player；不能把
本批描述为 shared barrier 或完整多人游戏。

## 本批实现

### 生产 server/client transport 与客户端协议状态机

- `multiplayer_client_transport` 复用 standalone Asio/TCP frame contract，提供 async DNS/IPv4/IPv6 resolve、connect/read/write、1 MiB frame cap、有界 inbound/outbound/pending-write 队列和安全 stop/join。
- 客户端入站 frame 只可占用 `inbound_event_count - 1` 个槽，给 disconnect/protocol/transport terminal event 留出控制槽；饱和时 fail closed，而不是静默丢失关键状态。
- 服务端为每个 admitted lifecycle 预留 connected 与 terminal control capacity；socket 关闭后逻辑 connection slot
  仍保持占用，直到 simulation thread 从 inbound queue 消费该连接的 disconnect/protocol/transport terminal
  event。这样 connect/reset churn 不能复用刚关闭的 slot 并把更早 terminal event 挤出有界队列；terminal event
  消费后才释放 admission capacity。若按该不变量本不应发生的 connected/terminal control enqueue 仍失败，
  transport 会先持久记录全局 fatal detail，再停止 accept/I/O；`poll_event()` 与 dedicated-server error path 均可
  取得该 detail，不依赖已经饱和的普通 inbound queue。
- `multiplayer_client` 实现 hello/auth/resume、session/identity/generation 校验、scene/revision tracking、ping/pong、resync、pending command cache 和原 payload replay；首次握手失败可重新 fresh auth，resume-expired 会清除旧 session 后允许 fresh retry。
- resume 的 `last_client_sequence` 不越过最早未确认 command；晚到 pong 不能确认前面的 command。缓存 command result 可以早于更新后的 resume full scene，但不能早于该 command 自己的 `base_revision`。
- 服务端以同一个 256-command window 约束执行结果缓存和 resume replay：只允许重放仍保留的
  `PlayerCommand` sequence；过旧 floor 会令 session 过期，先前的 ping、sequence gap、已淘汰 command、解析失败
  或 application queue 拒绝的 sequence 都不能在 resume 后跨类型复用为新 command。客户端提交的未来 floor 只会
  抬高 application high-water，不会重新开放旧 sequence。
- command result 会抬高客户端最小 scene revision；在对应 scene 到达前客户端不重新进入 ready，避免下一命令基于旧画面 revision。
- 本地 payload 构建或 enqueue 失败不会消耗 sequence，避免下一条消息在同一 TCP session 中制造业务序列缺口。
- typed `DisconnectNotice` 严格验证 rejection enum、UTF-8/control characters 和 512-byte message 上限；客户端等待 pending command 归零后请求 release，并要求 ACK session/sequence 精确匹配。服务端先把 leave request 放进 simulation FIFO，在所有更早 command/result/scene 排队后进入 releasing，并用一个原子的 transport 操作排入 ACK 与 ordered close。resume record 只在 ACK bytes 已写完且 transport 报告精确 ordered-close completion 后删除；peer close、queue/read/write failure 或任何非精确关闭都会保留 resume 能力。
- rejected auth/resume 允许空 session，accepted response 必须有非空 session。
- full semantic scene payload 增加 512 KiB 移动端预算；超限 snapshot 从最外 Chebyshev radius 向内裁剪并保留 center/player anchors，tile/entity 的每轴相对距离限制为 128，仍受 1 MiB transport cap 保护。

### 桌面 connection/input/rendering 边界

- `main.cpp` 新增 `--connect`、`--connect-token-file`、可重复 `--connect-mod` 和 `--allow-insecure-client-lan`。非 loopback plaintext 默认拒绝；token 必须来自 private regular file，POSIX mode 为 `0600` 或更严格。
- 客户端按服务器相同的 core + ordered mods 计算 SHA-256 content manifest。definition-only 临时 `WORLD` 只用于匹配 ordered mod-interaction loading，退出时通过 `worldfactory::set_active_world()` 恢复 options/world 指针；不创建 save、不加载服务器世界、不运行客户端权威模拟。
- `run_multiplayer_client_ui()` 使用本地 `input_context` 解析方向键、Windows keybinding 和 Android touch shortcut；只发送 typed wait/move command。当前 UI 保持一次一个未确认动作，不做客户端移动预测。
- UI 每 30 秒发送 ping，120 秒无 pong 时把 half-open transport 标记为断线；已经认证但等待 command 所需 scene revision 的状态也继续 heartbeat。恢复仍由本地 confirm 显式触发 resume/fresh retry，不包含自动 backoff。
- curses fallback 从本地 terrain/furniture/trap/monster definitions 绘制 semantic scene。窗口尺寸变化时重建 scene/status windows，退出时恢复原 `g->w_terrain`。
- SDL tiles 路径通过 `cata_tiles::draw_remote_scene()` 使用本地 tileset 绘制 terrain、furniture、visible trap、monster 和 player appearance；renderer 不查询客户端 map、monster tracker 或 save state。
- `tools/multiplayer/network_client_ui_smoke.py` 通过 PTY 和断线 TCP proxy 驱动真实 `cataclysm --connect`，已接入 transport workflow，验证 local wait/move/reconnect/quit 与 exactly-once replay。

### Android launcher 与私有凭据

- Splash screen 现在允许选择 single-player 或 multiplayer，并提供 endpoint、64 位小写十六进制 token 和显式 insecure-LAN 例外输入。
- endpoint 保存在 app preferences；insecure-LAN consent 每次启动都恢复为未选中。token 只可在保存时绑定的完全相同 endpoint 上复用，换地址必须重新输入。
- token 写入 `getNoBackupFilesDir()/multiplayer/client-token.txt`，经临时文件、flush/fsync、`chmod` owner-only、原子 rename、父目录 fsync 和 app-private canonical path 检查后替换。
- `CataclysmDDA.getArguments()` 只从应用内 Intent extras 生成 `--connect`、`--connect-token-file` 和可选 LAN 参数；token 值不进入 argv 或日志。
- launcher 对 incomplete multiplayer Intent 生成明确无效的 `--connect` 参数，让 native command validator fail closed，且不会回退启动本地游戏；SDL Activity 启动后不再重定向到 mode chooser。已有 crash/launch/connect dialog 不会在 Activity resume 时叠加，运行中的 native argv 也不会被 `onNewIntent()` 静默替换。
- Android CMake source glob 使用 `CONFIGURE_DEPENDS`，新增 `src/multiplayer_*.cpp` 会触发增量 reconfigure；arm64 与 x86_64 debug APK 都已实际编译新的 client、transport 和 SDL remote renderer。

### 额外加固与本地工具链诊断

- server token reader 先用 `symlink_status` 拒绝 symlink/非普通文件，且 POSIX mode/symlink 行为有回归测试。client reader 同样 fail closed，并额外执行 128-byte size cap 与 `0600` 检查；真实 PTY smoke 使用 private token file 覆盖其成功路径。
- 普通 release PTY 曾在 `initscr()` 报 `corrupted size vs. prev_size`。SIGABRT backtrace 证明本地无 root 工具链错误地把系统 shared `libncursesw.so.6` 与 prefix static `libtinfo.a` 混链，和 multiplayer client 无关。补齐 prefix 的 runtime library 后普通 release UI smoke 通过。
- `check-multiplayer-build-env.sh linux` 现编译一个同时引用 `initscr()`/`tparm()` 的 probe，通过 `ldd` 要求 ncursesw/tinfo 来自同一动态 runtime root，并用 `nm` 拒绝 `_nc_doalloc`/`_nc_tparm_analyze` 被静态带入；已人工移除 runtime symlink 验证该门禁会按预期失败。

## 验证证据

### Hosted Phase 1/baseline（已绿色）

- baseline run [`29219328448`](https://github.com/wsdx233/Cataclysm-DDA-Multiplayer/actions/runs/29219328448)，提交 `cd18703`，结论 `success`：
  - Linux curses artifact `8267944561`，digest `b4b2083d87190f8efada2c2d4e49e28370efc2b19f9bca0d2a31c010124e7dde`；
  - Windows x64 MSVC tiles+sound artifact `8267993011`，digest `cd291572d18ae2cf76eedbd07dc226d4ec211f51237e88f3044bc896fc21824f`；
  - Android arm64 release APK artifact `8267899979`，digest `981a9a6d3b8f6c4aa38f22dbbcfea1b9c76dbe1713eac0489049f222939a3e81`。
- transport/protocol run [`29219953446`](https://github.com/wsdx233/Cataclysm-DDA-Multiplayer/actions/runs/29219953446)，提交 `bc7efe0`，结论 `success`：
  - Linux GCC 13/Clang 18 + production game/process artifact `8268340815`，digest `ff04b8ecb4536e6e2bcc5a0aceaf7c04df8706c0aacf8dcda4f7a35998017d6e`；
  - Windows MSVC artifact `8267835930`，digest `e509b6579b874ce57cfbabda488cbff24a48c2757a0ba19a6d3f286a75eaaf78`；
  - Android NDK arm64 artifact `8267835710`，digest `1ea5da10ed8e951201d2cae95d58850b4edc982a69f52172ca2c0990ffb6ad5c`。

这些 runs 关闭 Phase 1 hosted gate，但早于本批 graphical/network client source；本批自己的 hosted 证据记录如下。

### Hosted Phase 2 network-client batch（已绿色）

- baseline run [`29303564150`](https://github.com/wsdx233/Cataclysm-DDA-Multiplayer/actions/runs/29303564150)，
  source `6403a949fb537be14ec4d5757f295adbf5c5f99b`，结论 `success`；Linux job `18m57s`、Android
  job `39m04s`、Windows job `32m34s`：
  - Linux curses artifact `8299663785`，digest `d9e228af65de0dcf52c4849a63a4de76267c8b1701de560518b13c01e4648069`；
  - Windows x64 MSVC tiles+sound artifact `8299880246`，digest `1ca0a557fc27235bda25b2579542c4f058c8c554561612ff37c13018e9289b44`；
  - Android arm64 release artifact `8299976619`，digest `d3d7382affbd7342951ca32957be2521165fbf2548f74e1d68d6ca22f86b4413`；
  - Android x86_64 debug artifact `8299977114`，digest `351094bbbd92664bb1245b586db978a257aed05daef6a51ae4316b8e39b3b16f`；
  - tileset artifact `8299414855`，digest `6403077a11ae9696a41780f162553f980fcb594f1395b2d0f8daac9844eaadde`；
  - shaders artifact `8299426143`，digest `098ede9628ca24e16ac8df6253ca8b88c26ce86e7d57fcd5048d274d674e92ee`；
  - soundpack artifact `8299403377`，digest `37749d3ac9d82f8199a57f6ce603e0785bf3151062e95c9c22177a8921cacee4`；
  - translations artifact `8299405280`，digest `b7ad074c2acb2ff9ae0fc9d7fae68cbb063082ec12332353c5f63f2b0156650b`。
- transport/protocol run [`29303564152`](https://github.com/wsdx233/Cataclysm-DDA-Multiplayer/actions/runs/29303564152)，
  同一 source，结论 `success`；Linux job `36m05s`、Windows job `58s`、Android job `33s`：
  - Linux GCC 13/Clang 18 + production game/process artifact `8299903967`，digest `0b38eaf73e9178895b8fe589573919582bf0144f6f7fa7346469966a14061013`；
  - Windows MSVC artifact `8299413066`，digest `f203a9ab7fb6d59aa65fe5ea671ae0bb90621bc7c77e4d59c23d775da21ea430`；
  - Android NDK arm64 artifact `8299407650`，digest `2c58a77c426dadf310948c945e64e25331dea2c7313ef2e62ab7effe41b37552`。

这些 terminal-success runs 关闭本批 hosted platform build gate，但 Android jobs 仍只是 package/compile evidence，
没有 emulator/真机 runtime lifecycle 结论。

### GCC 13 release

```bash
source build-scripts/activate-multiplayer-build-env.sh
make -j8 \
  COMPILER=g++-13 RELEASE=1 LOCALIZE=0 BACKTRACE=0 PCH=0 \
  cataclysm tests
./tests/cata_test '[multiplayer]' \
  --rng-seed 0 --user-dir /tmp/cdda-mp-final-client-suite-terminal-fix
```

结果：构建成功；53 test cases，其中 51 通过、2 个含完整 avatar move-swap reference-identity 对照的 cases 按既有 `[!mayfail]` 设计报告预期失败；3,066 assertions 中 3,063 通过、3 个为预期 `!mayfail`。新增覆盖包括 3,721 个长 ID tile 的 scene-budget 裁剪、fresh retry/expired resume、command-result scene floor、pending-scene heartbeat、256-command replay floor/cross-type reuse 拒绝、graceful ACK sequence、reserved control capacity、command → leave FIFO、result/scene → ACK write-drain/ordered socket close、所有失败路径的 resume 保留，以及 `maximum_connections=1`/`inbound_event_count=2` 下 closed logical slot 在 terminal event 被消费前拒绝 reset churn、消费后才允许 replacement connection 的回归。focused server_lobby/dedicated_server/transport 子集为 17 cases / 1,686 assertions，client 子集为 12 cases / 711 assertions，均全部通过。

### ASan/UBSan/LSan

```bash
make -j8 AUTO_BUILD_PREFIX=1 \
  COMPILER=g++-13 RELEASE=1 LOCALIZE=0 BACKTRACE=0 PCH=0 \
  SANITIZE=address,undefined \
  WARNINGS='-Wall -Wextra -Wno-error=array-bounds' \
  tests release-local-back-sanitize-cataclysm
ASAN_OPTIONS='detect_leaks=1:detect_stack_use_after_return=1:halt_on_error=1:abort_on_error=1' \
UBSAN_OPTIONS='print_stacktrace=1:halt_on_error=1' \
./tests/release-local-back-sanitize-cata_test '[multiplayer]' \
  --rng-seed 0 --user-dir /tmp/cdda-mp-final-client-sanitize
```

最终 source 的 sanitizer rebuild 成功；53 cases，其中 51 通过、2 个含既有完整 avatar move-swap 对照的 cases 按预期报告失败；3,066 assertions 中 3,063 通过、3 个为预期 `!mayfail`。无 ASan、UBSan、LSan 或 stack-use-after-return finding。诊断构建仍只对既有 GCC 13 initializer-list `array-bounds` optimizer warning 降级 `-Werror`，不是最终 warning policy 的变化。

### 真实 network-client UI / resume

最终 source state 的普通 release 与 ASan/UBSan binary 都通过 `tools/multiplayer/network_client_ui_smoke.py`：

```bash
python3 tools/multiplayer/network_client_ui_smoke.py \
  --client "$PWD/cataclysm" \
  --backend-port 38265 \
  --token-file /tmp/cdda-mp-ui-smoke-final-release-terminal-fix/server-auth-token.txt \
  --user-dir /tmp/cdda-mp-ui-smoke-final-release-terminal-fix/client-user \
  --transcript /tmp/cdda-mp-ui-smoke-final-release-terminal-fix/client.transcript \
  --event-log /tmp/cdda-mp-ui-smoke-final-release-terminal-fix/client-events.txt

ASAN_OPTIONS='detect_leaks=0:detect_stack_use_after_return=1:halt_on_error=1:abort_on_error=1' \
UBSAN_OPTIONS='print_stacktrace=1:halt_on_error=1' \
python3 tools/multiplayer/network_client_ui_smoke.py \
  --client "$PWD/release-local-back-sanitize-cataclysm" \
  --backend-port 37263 \
  --token-file /tmp/cdda-mp-ui-smoke-final-sanitize/server-auth-token.txt \
  --user-dir /tmp/cdda-mp-ui-smoke-final-sanitize/client-user \
  --transcript /tmp/cdda-mp-ui-smoke-final-sanitize/client.transcript \
  --event-log /tmp/cdda-mp-ui-smoke-final-sanitize/client-events.txt \
  --timeout-seconds 180
```

两个 matching server fixtures 分别由同一个 release/sanitizer binary 在上述 `/tmp` 根目录中以
`--init-server-config` 后接 `--server` 启动；实际 loopback backend ports 已按最终 `server.json` 记录在命令中。

1. 真实 dedicated server 完成 hello/auth 和初始 visibility-filtered scene；
2. PTY 本地 `.` 解析为 semantic wait；proxy 在命令 bytes 已转发后断开；
3. 本地 confirm 发起 resume，服务器返回新 session generation 和 full scene；
4. 客户端原 payload 重放得到 duplicate result，不重复执行 wait；
5. PTY 本地 `l` 解析为 east move，收到 command result 与新 scene；
6. PTY 本地 quit 等待 pending command settlement，server 按 simulation FIFO 排入 typed release ACK，在 ACK write drain 与 ordered socket close 完成后删除 resume record，客户端随后干净退出；最后 server SIGTERM 保存并 shutdown。

release 与 sanitizer 的 command statuses 都精确为 `accepted (0), duplicate (2), accepted (0)`，两次客户端均返回 0，transcript 均为 7,503 bytes；events 都精确为 `listening`、`player_authenticated`、`command_result`、`player_disconnected`、`player_resumed`、`command_result`、`command_result`、`player_disconnected`、`save_completed`、`shutdown`。sanitizer transcript 不含 ASan/UBSan/LSan marker。第一次 sanitizer 尝试只超过旧的 180 秒 server-startup allowance，并非 sanitizer finding；fresh-root 重试把 sanitizer server startup allowance 放宽到 600 秒，客户端交互的 `--timeout-seconds 180` 保持不变，随后完整通过。sanitizer process smoke 使用 `detect_leaks=0`，因为 ncurses 本身在进程退出时有已知约 1.9 KiB library leak。server stdout/stderr 每个非空行均为无 ANSI 的合法 JSON。

### Android arm64 / x86_64 debug APK

```bash
source build-scripts/activate-multiplayer-build-env.sh
cd android
./gradlew -Pj=8 \
  -Pabi_arm_32=false -Pabi_arm_64=true \
  -Pabi_x86_32=false -Pabi_x86_64=false \
  -Plocalize=false assembleExperimentalDebug
```

最终 source state 的 arm64 构建为 `BUILD SUCCESSFUL in 1m 21s`，39 tasks（13 executed、26 up-to-date），APK SHA-256 为 `0f6d7324675c4291281d072ba631ab7a2477d9507033eefb3102b9fec6e6da6b`，且只包含 `arm64-v8a/libmain.so`。

x86_64 使用同一命令但令 `-Pabi_arm_64=false -Pabi_x86_64=true`，结果为 `BUILD SUCCESSFUL in 1m 19s`，39 tasks（13 executed、26 up-to-date），APK SHA-256 为 `06e70f034bd9761aa98f6194798ffcc8706659ec387ebc590494340af1af66d1`，且只包含 `x86_64/libmain.so`。更早一次 x86_64 Java compile 因 Android API 没有 `OsConstants.O_DIRECTORY` 失败；改为以 `O_RDONLY` 打开 app-private 父目录再 fsync 后，连续 x86_64/arm64 构建均绿色。

两份 APK ZIP 都完整、声明 `android.permission.INTERNET`，并包含 `connectMultiplayer`、`launchModeTitle`、
`multiplayerConnectTitle`、endpoint/token validation、`multiplayerExistingSession`、LAN consent/security notice 和
token-storage resources。该证据证明 Java/C++/SDL tiles source integration，不是 emulator/真机连接证据。

### Schema、依赖、格式和 workflow

```bash
source build-scripts/activate-multiplayer-build-env.sh
make ASTYLE_BINARY="$HOME/.cache/cdda-tools/astyle-3.1-3build1/root/usr/bin/astyle" astyle-check
python3 -m py_compile tools/multiplayer/network_client_ui_smoke.py
bash -n build-scripts/check-multiplayer-build-env.sh
PATH="$HOME/.cache/cdda-tools/shellcheck-0.9.0-1/root/usr/bin:$PATH" \
  "$HOME/.cache/cdda-tools/shellcheck-0.9.0-1/root/usr/bin/shellcheck" \
  build-scripts/check-multiplayer-build-env.sh
PATH="$HOME/.cache/cdda-tools/shellcheck-0.9.0-1/root/usr/bin:$PATH" \
  "$HOME/.cache/cdda-tools/actionlint-1.7.12/actionlint" \
  .github/workflows/multiplayer-baseline.yml \
  .github/workflows/multiplayer-transport-spike.yml
python3 - <<'PY'
import pathlib
import subprocess
import tempfile
import yaml

for path in [pathlib.Path('.github/workflows/multiplayer-baseline.yml'),
             pathlib.Path('.github/workflows/multiplayer-transport-spike.yml')]:
    data = yaml.safe_load(path.read_text())
    scripts = []
    for job_name, job in (data.get('jobs') or {}).items():
        for idx, step in enumerate(job.get('steps') or []):
            run = step.get('run') if isinstance(step, dict) else None
            shell = step.get('shell', '') if isinstance(step, dict) else ''
            if isinstance(run, str) and ('bash' in shell or not shell):
                scripts.append((job_name, idx, run))
    for job_name, idx, script in scripts:
        with tempfile.NamedTemporaryFile('w', suffix='.sh') as source:
            source.write(script)
            source.flush()
            result = subprocess.run(['bash', '-n', source.name], text=True,
                                    capture_output=True)
            if result.returncode:
                raise SystemExit(f'{path}:{job_name}:step {idx}: {result.stderr}')
    print(f'{path}: YAML parsed; {len(scripts)} Bash-compatible run blocks passed bash -n')
PY
./build-scripts/check-multiplayer-build-env.sh all
git diff --check
```

- FlatBuffers 1.12.0 archive SHA-256 已核对，`generate.sh --check` 通过；GCC 13 只使用 `-Wno-error=stringop-overflow` 处理固定第三方 optimizer false positive。
- Asio `asio-1-38-1` archive、license 和 vendored headers 逐字节一致。
- AStyle 3.1 `make astyle-check`：通过。
- ShellCheck 0.9.0：changed Bash 通过；actionlint 1.7.12：两个 workflow 通过。actionlint 首次发现两个仅作计数的 `attempt` loop variable 为 SC2034，改为 `_` 后复检绿色；本地仍没有 PowerShell parser，MSVC script 最终由 hosted Windows job 验证。
- workflow YAML parse：通过；baseline 17 个、transport 13 个 Bash-compatible `run:` blocks 经 `bash -n`：通过。
- `python3 -m py_compile tools/multiplayer/network_client_ui_smoke.py`、`bash -n build-scripts/check-multiplayer-build-env.sh`、`git diff --check`：通过。
- `./build-scripts/check-multiplayer-build-env.sh all`：通过；Linux ncursesw/tinfo runtime 与 Android pinned SDK/NDK/JDK 均绿色。

## 已知限制和未完成项

- Android 尚无 emulator/真机 auth/render/wait/move、Activity pause/resume 和网络切换 smoke；这些是当前 Phase 2 设备证据缺口。
- remote scene 仍只有 full snapshot；512 KiB fitter 会有损缩小可见半径，尚无 delta/chunk/compression 或 reduced-viewport metadata。scene 已携带 lighting byte，但 curses/tiles renderer 暂按全亮绘制；isometric terrain/entity painter ordering 也未完成。items、fields、vehicles、overlays、messages、sound、avatar replica/panels 仍缺失。
- visibility regression 目前覆盖遮挡怪物不泄漏，但 ADR-0006 要求的隐藏陷阱、未探索地形、不可见物品以及 delta 路径 leak matrix 尚未完成；其中 items/delta 也尚未实现。
- heartbeat 目前只有 30 秒 ping、120 秒 timeout 与手动 confirm reconnect；Android background timing、half-open recovery、自动 retry/backoff 和 graceful-disconnect timeout 尚无运行证据。若服务端在 clean release 时不回 ACK，UI 仍需第二次 quit 才能强制离开。
- durable process-restart resume 属于 Phase 4：resume token、pending command 和 scene revision 当前仍只在 native process 内存中，进程杀死后不能继续旧 session；后续需 app-private、版本化、原子 checkpoint，但这不是 Phase 2 或 Phase 3 的进入门禁。
- clean `DisconnectNotice` 已验证 command settlement、ACK write drain、ordered close 以及仅在精确 completion 后清除 resume record；但多数 protocol/auth/application 错误仍通过 transport close reason 而非 typed disconnect payload 返回。
- client token reader 的 size/mode/symlink 检查仍存在 path-check → open 的 TOCTOU 窗口；Windows private-file ACL 尚未由本地平台证据验证。
- 服务器仍只有一个 `active_remote_session`，config 继续拒绝 `players.max > 1`；没有 ADR-0002 shared scheduler、第二 avatar、tether/group shift、多人 monster target 或 player-state isolation。
- save 仍是 canonical single-avatar generation；没有 multi-player runtime split、RNG engine save、generation fallback 或 portable character。
- 当前动作只有 wait 和八方向平面 move。所有其他动作必须保持明确未支持，不能进入 server blocking UI。
- 无嵌入式 TLS；loopback 默认、trusted-LAN 显式例外和外部 authenticated tunnel 政策保持不变。

## 下一门禁和首个动作

1. hosted platform gate 已齐；首个具体动作是执行：

```bash
adb devices -l
```

   在可用 emulator/真机上安装匹配 ABI 的 debug APK，对真实 server 跑 auth/render/wait/move、Activity
   pause/resume、强制 network disconnect 和 reconnect 最小 smoke。第一代码检查点是
   `android/app/src/main/java/com/cleverraven/cataclysmdda/SplashScreen.java` 与 `CataclysmDDA.getArguments()` 的
   Activity lifecycle/Intent persistence。
2. Android lifecycle 退出证据齐全后，才扩大 server config 到第二玩家。Phase 3 第一条代码调查命令仍是：

```bash
rg -n "do_turn_remote|do_turn_impl|active_remote_session|multiplayer_players|active_player_guard|all_monsters|monmove" \
  src/do_turn.cpp src/game.cpp src/game.h src/main.cpp src/multiplayer_* tests/multiplayer_*
```

不得把本批单客户端 UI smoke 描述为两玩家/shared-barrier、完整 remote avatar replica、portable character 或生产发布完成。
