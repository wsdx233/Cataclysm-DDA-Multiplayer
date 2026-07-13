# CDDA 多人 fork 当前状态

- 更新日期：2026-07-13
- 分支：`multiplayer/main`
- 当前阶段：**Phase 2，单远程玩家垂直切片**；Phase 1 的本地退出门禁已满足，当前提交的 hosted
  Linux/Windows/Android 结果仍待推送后记录
- 当前基线父提交：`9860e01`（`test: establish multiplayer turn phase boundaries`）
- 上游基线：`d84b90dd2aee090ca28c8dad5cdf1fab6dea151a`

## 当前结论

Phase 0 关闭结论保持不变：地址稳定的 avatar/player-runtime bridge、turn phase 边界、standalone Asio
三平台 spike 和 baseline artifacts 均已有证据，ADR-0001 至 ADR-0009 的当前决策未被本批实现推翻。

Phase 1 的实现和本地退出标准现已完成：真实游戏 binary 可以在无终端交互、无 curses/SDL/ImGui/sound
初始化的 dedicated mode 中创建/加载世界、监听、严格握手、拒绝不兼容客户端、处理 signal、保存并退出。
生产 transport 位于 `src/multiplayer_transport.*`，不是 `tools/` spike 的扩展。

Phase 2 已形成**服务器侧单远程玩家纵向切片**：一个 headless test client 可以控制服务器唯一
server-owned avatar 的 `wait`/八方向平面 `move`，获得 visibility-filtered full scene、revision、command
result、ping/pong、resync、disconnect/resume 和 exactly-once replay 行为。尚无 Windows/Android 图形 network
client、remote avatar replica 或本地 tiles/touch renderer，因此不能宣称 Phase 2 退出标准已经完成。

## 本批实现

### Headless runtime、配置和 canonical bootstrap

- `multiplayer_runtime_mode` 提供 `local_client`、`network_client`、`dedicated_server`、`test` 边界；当前 CLI
  实现 `--init-server-config`、`--check-server-config` 和 `--server`。
- schema 1 config 使用严格 unknown-member/path/security/resource 校验；默认只监听 `127.0.0.1:27999`。
  plaintext 非 loopback 必须显式声明 trusted-LAN 例外，WAN 仍要求外部认证加密隧道。
- config/token 使用 exclusive create，POSIX 生成权限为 `0600`，不会覆盖已有文件；token 读取拒绝 group/other
  权限和非法格式。
- 当前只交付一个 remote player、`server_owned` 和单一 canonical save generation，因此 parser 明确拒绝
  `players.max > 1`、`portable_lease`、`copy_in` 和 `save.keep_generations > 1`，避免配置伪装为已实现能力。
- dedicated bootstrap 使用配置的 world seed/mod order/options，创建或加载唯一 avatar save；SIGINT/SIGTERM、
  game over 和 `save.interval_turns` 都进入服务器保存路径。重启 smoke 已证明保存后加载同一个 avatar save。

### Headless/UI 隔离和 turn loop

- `game::do_turn()` 与 `game::do_turn_remote()` 共用 `do_turn_impl()`；remote callback 只在 simulation thread 的
  player-input phase 执行，transport/lobby 不直接接触 live game objects。
- remote path 跳过 renderer recovery、music/SFX、autosave、截图、blocking activity input、progress UI、
  `FORCE_REDRAW` 和本地 game-over cleanup。dedicated runtime 统一抑制 explosion/bullet/hit 动画，并抑制
  popup、debug prompt 和 loading UI。
- local pause/move 与 remote wait/move 共用 `multiplayer_command_executor`。remote move 在进入 legacy move
  executor 前拒绝 safe mode、remote control、车辆驾驶、友好 NPC menu；legacy executor 还以
  `allow_interactive_ui=false` 拒绝高速跳车和需要确认的深水进入，不进入 `query_yn()`、NPC menu 或 peek UI。
- 当前循环只有一个 active remote session 和一个 server-owned active avatar；它不是 ADR-0002 的多玩家
  shared turn barrier，也没有跳过 Phase 3 的 scheduler/world-phase ownership 工作。

### Transport、协议、身份和幂等性

- vendored standalone Asio 固定为 `1.38.1`，生产 server transport 使用 1 MiB frame cap、有界 inbound/
  outbound queues、per-connection write bounds、半关闭处理和有序 shutdown；I/O worker 只交换 immutable bytes/DTO。
- FlatBuffers schema 固定用 `flatc 1.12.0` 生成。hello 严格比较 protocol major/minor、build ID、ordered
  content manifest、server-state schema、savegame version 39 和 required capabilities。
- content manifest 对 `data/core` 和配置 mod order 的每个普通文件按 root ID、规范相对路径、长度和内容计算
  deterministic SHA-256；拒绝 symlink 和非普通文件。真实 `dda` manifest 覆盖 3,000+ files、40 MiB+ 数据。
- bearer auth 返回 canonical server player UUID/character ID；resume 轮换 session ID 并递增 generation。
  lobby 强制递增 envelope sequence、认证/消息/字节限流和有界 simulation event queue。
- command cache 保留最近 256 个结果。resume 可重放不确定的最后命令；相同 sequence+相同 payload 返回
  `duplicate`，相同 sequence+不同 payload 记录 `command_sequence_conflict` 并断开。fresh auth/旧 token 到期后
  开始新的 sequence epoch。
- stale `base_revision` 返回 typed rejection。有效 action 发布 action revision，随后完整 world phase 再发布
  post-world revision；resync payload 经过边界和 UTF-8/control-character 校验，client revision 不得领先 server。

### Visible scene 和日志

- full scene 只包含请求半径内且 authoritative avatar 实际可见的 terrain、furniture、可见 trap、player 和
  monster；墙后 monster 不泄漏。player/monster health、attitude、absolute position 和 appearance ID 来自
  server state。
- monster 使用 creature-tracker shared ownership 驱动的进程内稳定 scene ID；增加其他 monster 不会改变既有 ID。
- server stdout/stderr 为单行 JSON：startup/listening/auth/resume/disconnect/resync/command/save/shutdown/error。
  command 日志包括 player、client sequence、command type、status/rejection、revision、moves 和 duration；
  不记录 bearer/resume token。

## 验证证据

### GCC 13 release

```bash
source build-scripts/activate-multiplayer-build-env.sh
make -j"$(nproc)" \
  COMPILER=g++-13 RELEASE=1 LOCALIZE=0 BACKTRACE=0 PCH=0 \
  cataclysm tests
./tests/cata_test '[multiplayer]' \
  --rng-seed 0 --user-dir /tmp/cdda-mp-final-suite
```

结果：构建成功；30 test cases，996 assertions，其中 993 通过，恰好 3 个完整 avatar move-swap reference
identity assertions 按既有 `[!mayfail]` 设计失败。其余 28 cases 全部通过。

### ASan/UBSan/LSan

```bash
make -j8 AUTO_BUILD_PREFIX=1 \
  COMPILER=g++-13 RELEASE=1 LOCALIZE=0 BACKTRACE=0 PCH=0 \
  SANITIZE=address,undefined \
  WARNINGS='-Wall -Wextra -Wno-error=array-bounds' tests
ASAN_OPTIONS='detect_leaks=1:detect_stack_use_after_return=1:halt_on_error=1:abort_on_error=1' \
UBSAN_OPTIONS='print_stacktrace=1:halt_on_error=1' \
./tests/release-local-back-sanitize-cata_test '[multiplayer]' \
  --rng-seed 0 --user-dir /tmp/cdda-mp-final-sanitize
```

结果：同样为 30 cases / 996 assertions / 3 个预期 `!mayfail`；无 ASan、UBSan、LSan 或
stack-use-after-return finding。构建时 GCC 13 对两个既有 initializer-list 路径产生 `-Warray-bounds`
优化告警，因此 sanitizer 专用构建关闭该告警的 `-Werror`，未修改上游业务代码。

### 真实 server/client 进程和 restart

最终 smoke 根目录：`/tmp/cdda-mp-final-process.J0QcHP`。

`tools/multiplayer/headless_client_smoke.cpp` 对真实 `./cataclysm --server` 依次验证：

1. incompatible hello typed rejection，再以 server compatibility axes 完成 hello；
2. token auth、canonical identity、initial full scene 和 explicit resync；
3. authoritative wait、accepted command result、action scene 和 post-world scene；
4. disconnect/resume、相同 payload duplicate replay；
5. 相同 sequence 不同 payload 被断开；
6. SIGTERM save/shutdown；同一 userdir 第二次启动加载已有 avatar save，再次干净保存退出。

两次 stdout/stderr 每个非空行均可由 JSON parser 读取且没有 ANSI escape。默认 config/token 均为 `0600`，
默认 `players.max=1`、`character_policy=server_owned`、`keep_generations=1`。第二次 debug log 包含
`Loading existing dedicated multiplayer avatar save.`，世界目录中仍只有一个 `.sav.zzip`。
缺失 config 的 `--server` 失败路径也返回 status 1 和单行 `startup_failed` JSON。

### Android arm64

```bash
source build-scripts/activate-multiplayer-build-env.sh
./build-scripts/check-multiplayer-build-env.sh android
cd android
./gradlew -Pj="$(nproc)" \
  -Pabi_arm_32=false -Pabi_arm_64=true \
  -Pabi_x86_32=false -Pabi_x86_64=false \
  -Plocalize=false assembleExperimentalDebug
```

结果：环境门禁通过；最终构建 `BUILD SUCCESSFUL in 6m 6s`，39 tasks（8 executed、31 up-to-date）。
这证明当前 shared C++ source 进入 Android NDK arm64 target；不是 Android 图形 network-client 运行证据。

### 依赖、schema 和静态检查

- `FLATC=/tmp/flatbuffers-1.12.0-build/flatc tools/multiplayer/protocol/generate.sh --check`：通过。
- pinned Asio archive SHA-256 为
  `2827b229972be80cdb14e5497962fa393d1adf036b5869e2b9c99f644daadacc`；vendored headers/license 与 archive
  `cmp`/`diff -qr` 一致。
- AStyle 3.1 对全部 changed non-vendored C++ dry-run 无输出；`git diff --check` 通过。
- workflow YAML 可解析，所有 Linux `run:` block 经 `bash -n` 通过。环境没有本地 `actionlint` binary；
  hosted workflow parser 结果需在推送后记录。

## Hosted 基线

已验证父提交 `a39e06e` 的历史证据保持有效：baseline run `29205262759` 和 transport run
`29205262750` 已覆盖 Linux curses、Windows MSVC、Android arm64 及 standalone transport。它们不能替代
本批生产 source 的 hosted 编译证据。

当前 workflow 已扩展为：Linux GCC 13 构建真实 `cataclysm`/`tests`、运行完整 `[multiplayer]`、编译 smoke
client 并执行真实 process auth/scene/command/resume/save；同时保留 GCC/Clang/Windows/Android transport
和 FlatBuffers regeneration gates。推送本批提交后必须在本节记录新的 baseline/transport run ID、结果和失败。
在证据出现前，不把 Phase 1 的 hosted 平台门禁标记为关闭。

## 已知限制和明确未完成项

- **没有图形 network client**：Windows/Android 目前不能 connect/render remote scene；Phase 2 尚未关闭。
- **只有一个 remote player**：没有第二 human avatar scheduler、round-robin barrier、tether/group shift、
  双玩家冲突、monster multi-human targeting 或 per-player message/safe-mode isolation。配置会拒绝 `max > 1`。
- scene 只有 full snapshot；没有 delta/chunk/compression、items/fields/vehicles/overlays、per-player messages 或
  sound events。当前半径 30 的 full scene 必须保持在 1 MiB frame cap 内。
- save 使用现有 canonical single-avatar `game::save()`；没有 generation manifest-last、RNG engine state、
  multi-player barrier save、kill -9/disk-full recovery 或 old-save migration matrix。
- `portable_lease`/`copy_in`、ID/item UID remap 和人物包签名未实现并被配置拒绝。
- 没有嵌入式 TLS。默认 plaintext 仅 loopback；trusted LAN 需显式配置，WAN 需要外部 authenticated tunnel。
- hosted MSVC 和 baseline artifacts 对当前 source 尚未验证；这是当前唯一平台证据缺口，不是可忽略失败。

## 下一门禁和首个动作

1. 推送当前 source，等待并记录 `multiplayer-baseline` 与 `multiplayer transport/protocol gates` 的完整结果；
   任一 job 失败先修复，不关闭 Phase 1 hosted gate。
2. Phase 2 下一实现门禁是建立生产 `multiplayer_client` connection/replica 边界，并先做 Windows graphical
   client 的 local input → semantic command、scene → local renderer smoke；不得让客户端调用 game rules。
3. 随后接 Android touch/local tiles renderer、app pause/resume 和 reconnect smoke，完成 Phase 2 平台退出标准。
4. 只有 Phase 2 客户端闭环和平台证据完成后才进入 Phase 3；第一条 scheduler 调查命令为：

```bash
rg -n "do_turn_remote|do_turn_impl|multiplayer_players|active_player_guard|all_monsters|monmove" \
  src/do_turn.cpp src/game.cpp src/game.h src/multiplayer_* tests/multiplayer_*
```

不得把当前 single-avatar process smoke 描述为两玩家/shared-barrier 或完整游戏客户端。
