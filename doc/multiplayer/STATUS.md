# CDDA 多人 fork 当前状态

- 更新日期：2026-07-15
- 分支：`multiplayer/main`
- 当前 source 基线：`cbd19b48d652be735a3c83fe841d2d7c831aecba`
- 上游基线：`d84b90dd2aee090ca28c8dad5cdf1fab6dea151a`
- 当前阶段：**Phase 3，第二玩家/shared scheduler 的 production integration**
- 当前 active gate：**Gate 2，production single-root lifecycle、scheduler owner 与 actual world claim**
- ADR 状态：ADR-0001 至 ADR-0009 已接受；ADR-0010 仍为**待验证**
- 配置约束：`players.max = 1`，在本文件列出的 lifecycle、production owner 和规则门禁关闭前不得提高
- 当前验证选择：后续内部 lifecycle/scheduler/game-rule 工作默认 **Tier 1 Linux**
- 当前 hosted source 证据：Gate 1 source 的 Linux production run `29392575820` terminal `success`；protocol
  public-boundary baseline run `29385561653` 仍覆盖 source `eb990c4ad9975915336f3acd65431b47d123e842`

## 当前结论

Phase 0、Phase 1 和 Phase 2 已关闭；对应平台、Android lifecycle、构建产物和历史 run 证据见
[`MULTIPLAYER_BUILD_BASELINE.md`](MULTIPLAYER_BUILD_BASELINE.md)、
[`MULTIPLAYER_REFACTOR_PLAN.md`](MULTIPLAYER_REFACTOR_PLAN.md) 和 Git 历史。本文件只保留当前 Phase 3 快照。

当前生产单玩家 server 已使用 simulation-thread-only `multiplayer_session_directory` 取代旧的
`active_remote_session`。lobby auth/resume 使用两阶段 admission，directory/runtime 是 identity、binding 和
generation 的权威；lobby token record 只是受控 wire/replay mirror。protocol minor `1`、客户端 last accepted
generation、normal resume 精确 `+1`、lost-response exact-fingerprint replay、exact-tuple application confirmation、
fresh/terminal token 回收和 ordered rejection 已接入并取得本地 Linux 及 hosted 证据。

Phase 3 Gate 1 已在 source `cbd19b48d652be735a3c83fe841d2d7c831aecba` 关闭。新增纯
`multiplayer_selected_root_lifecycle`，分别记录 transport connection、barrier、runtime、departure 和 server
dormant/fault 状态；departure ticket 绑定 owner/version/epoch，admission recipe 只绑定 lifecycle owner/version，
不是 directory commit receipt。directory
现在支持 exact runtime offline、graceful-release-pending 和由 commit 决定的 unpublished cleanup；runtime 支持同一
generation reactivation；scheduler 支持 same-generation replay rebind 和保持 disconnect grace 的 `+1` generation
repair。状态表证明 forced wait/world/offline 与 caller-reported ACK-queued/early-close 顺序、timeout/resume 单赢家、
same stable runtime
reactivation、safe guard/save 边界和 fail-stop 合约。

当前 source 还不是 production shared scheduler：

- `multiplayer_turn_scheduler` 和 `multiplayer_turn_phase_adapter` 已有 Linux 单元、故障注入和 sanitizer 证据，但
  adapter 仍只由测试调用。
- `multiplayer_selected_root_lifecycle` 是不拥有 socket/runtime/scheduler/game object 的纯跨组件契约；production
  dedicated owner 尚未执行并记录其 effects，不能把 Gate 1 描述为 production dormant 已接通。
- `do_turn_impl()` 仍直接调用 actual `process_legacy_single_player_bubble_turn()`；真实 world phase 尚未置于
  `claim_world()`/`record_world_completed()` 之间。
- transport disconnected、barrier disconnected、forced wait、terminal barrier、runtime offline 和 server dormant
  虽已有可组合 contract/API，尚未由同一个 production owner 串联。
- selected root 仍必须保持 active 才能创建 guard；正确方案是完成当前 barrier 和 actual world 后才 offline，
  dormant 时不再启动 turn，resume 先激活同一稳定 runtime 再解除 dormant。
- execution fault 目前只在 scheduler/adapter 内 latch；production 尚未把它映射为 typed fatal shutdown 和
  side-effect-aware no-save policy。
- 第二 client、双玩家 production routing 和完整多人规则矩阵尚未实现，因此 `players.max` 必须保持 `1`。

## 当前 ownership 快照

| 层 | 已完成 | 当前缺口 |
| --- | --- | --- |
| Transport/lobby | connection、token mirror、pending admission、ordered close/rejection | disconnect 事件尚未驱动完整 barrier/runtime lifecycle |
| Session directory | stable identity/binding/generation、two-stage commit/confirmation、graceful pending、exact offline、commit-owned unpublished cleanup | current-turn terminal 后的 offline/dormant transition 尚未接入 production |
| Registry/runtime | 地址稳定 avatar/runtime、active/offline/dead、exact offline/reactivation、active-player guard | selected root 的在线生命周期仍未由 production loop 驱动 |
| Scheduler | roster snapshot、round-robin、disconnect grace、forced-wait pending、replay/repair、world ticket、fault latch | 尚无 production owner/caller |
| Phase adapter | scoped target guard、真实 action/wait、bookkeeping、world callback contract | 尚未接管 production command 和 actual bubble |
| Root lifecycle | owner/version-bound pure state contract、effect recipe、guard/save/fault gate | 尚未绑定 production directory/scheduler/transport/game effects |
| Dedicated owner | authoritative directory 与真实 transport 已接通 | 尚未持有 scheduler/adapter、dormant loop 和 fatal/no-save recovery |

## 当前实现边界

### Authoritative session directory

- auth/resume 顺序为 `pending -> plan -> pre-encode -> directory commit -> transport enqueue -> lobby publish`。
- accepted response enqueue 失败只清 binding，不回滚已消耗 generation。
- normal resume 只允许 `old -> old + 1`；所有相关 generation 必须满足 `0 < generation < INT64_MAX`。
- accepted resume response 丢失时，只允许同 token、旧 generation、revision 和 client sequence fingerprint 重放已提交代。
- 第一条有效 application frame 触发 exact-tuple confirmation；directory 接受后，server 才 ack lobby mirror 并消费
  authoritative replay permission。
- application、graceful disconnect 和 stale disconnect 均校验 exact connection/session/player/character/generation tuple。
- admission commit 记录 unpublished cleanup 应恢复 offline 还是保持 active，调用方不能传入 cleanup mode；accepted
  enqueue failure 走 `record_admission_unpublished()`，generation 仍不回滚。
- graceful-release-pending 清除 commandable binding 但保留 exact tuple，允许 barrier/world/offline 完成后 ACK，或在
  ACK 前 transport early close 时精确清理。
- `record_runtime_offline()` 只在 simulation thread、无 active-player guard、无 commandable binding 且 exact
  player/character/generation/runtime mirror 一致时推进；duplicate offline 幂等返回。

### Scheduler/adapter seam

- scheduler 是 simulation-thread pure policy，不拥有 avatar、socket、command payload 或 gameplay callback。
- adapter 在目标 active-player guard 内执行规则和 action bookkeeping，退出 guard 后才记录 scheduler。
- authoritative wait 绕过玩家请求的 safe-mode permission，但执行真实 `Character::pause()`。
- callback、context restore、post-side-effect record 或 world callback failure 会 latch execution fault，拒绝 replacement
  adapter 和后续 transition。
- in-process 两 runtime wait-only barrier 已绿色，但它不是 production 双玩家 server 证据。

### Selected-root lifecycle contract（Gate 1 已关闭）

- connection、barrier、runtime、departure 与 server state 独立；connected turn 和 departure turn 都必须完成 exact
  shared-turn world claim/record，旧 turn terminal/world completion 不能推进新 turn。
- disconnect deadline 与 resume 只有一个赢家；timeout 一旦进入 forced-wait pending，resume 延后到 dormant；grace
  内 published `+1`、same-generation replay 和 unpublished `+1` repair 使用不同 scheduler effect。
- departure ticket 绑定 lifecycle owner/epoch/version。unpublished repair 后 departed transport binding 可保持旧代，
  但 participant/runtime key 使用已提交 canonical generation；旧 ticket 立即 stale。
- caller-reported graceful ACK queued 只能在 runtime offline 后记录；ACK 前 caller-reported early close 取消 ACK
  obligation，但仍须完成已开始的 barrier/world/offline，再允许 resume。actual transport enqueue/close result 由
  Gate 2 owner 证明。
- dormant 不开始 turn、不创建 guard、不执行 command；clean turn boundary 或 dormant 才允许 save/shutdown。
- 该对象只验证调用方见证的顺序，不证明外部 effect 已执行。Gate 2 owner 若已成功执行外部 effect，而匹配
  `record_*` 不是 applied/duplicate，必须立即 `latch_fault()`；不得重试可能非幂等的 side effect。

## 当前验证策略

规范性分层规则见重构计划第 20.6 节；这里仅记录当前 Phase 3 批次的选择和频率。

### 当前选择：Tier 1 Linux

接下来的 selected-root lifecycle、production scheduler owner、`do_turn()` seam 和 game-rule 工作属于
backend-neutral internal shared implementation。只要 diff 不改变 wire/schema/public ABI、不触及平台 conditional，
也不修改 Windows/Android-owned build、UI 或 lifecycle，Linux headless server 与 native client 足以作为日常功能证据。

验证频率分为三档：

1. **编辑循环**：增量构建受影响的 Linux target，运行 changed-area focused tests。不要每次重跑完整
   `[multiplayer]`、sanitizer、PTY 或任何平台 package。
2. **切片收口**：authority/session/turn invariant 改变时运行一次完整 `[multiplayer]`；涉及地址、生命周期、线程或
   所有权时，再运行一次定向 ASan/UBSan/LSan。
3. **Production 接通**：command/session/disconnect/world path 真正进入 server owner 后，运行一次真实 Linux
   `--server` + native curses client PTY/loopback；触及 Linux SDL renderer 时改用 Linux graphical client。

Windows/Android 只在以下情况升级：

- 修改 Windows/Android-owned build、runtime、UI、SDL/touch、Activity/JNI/ABI/resource 或平台 conditional；
- 修改 wire schema/version/capability、公开 DTO/serialization layout、compiler/ABI-sensitive public header、共享
  source-list 或 pinned toolchain contract；
- 进入 release、明确跨平台兼容里程碑，或 Phase exit 审计确认已有平台证据不能覆盖候选 diff。

Tier 2 使用能实际编译 changed production source 的最小平台 gate。完整 Windows package、Android APK 或
emulator/device 仅在 package/resource/runtime/lifecycle 本身是验收目标时运行。transport workflow 的 isolated
Windows/Android spike 不能作为 scheduler/adapter 或其他 production source 的平台证据。

Phase exit 前必须对候选 commit 与最近可继承的平台证据执行 commit/diff audit。只有受影响平台的 owned code、
public boundary、source-list、toolchain、platform conditional 和相关平台产品声明均无不兼容变化，既有证据仍覆盖
退出声明，并在 handoff 中记录 source/run/audit 结论时，才可继承该证据；否则只补受影响平台的 Tier 2/3 gate。
不得因为 phase exit 名称机械重跑无关平台，也不得把旧证据冒充新代码证据。

## 当前 source 验证证据

### 当前 Gate 1 source 的本地 Linux 收口

source `cbd19b48d652be735a3c83fe841d2d7c831aecba` 执行过：

```bash
source build-scripts/activate-multiplayer-build-env.sh
make -j"$(nproc)" AUTO_BUILD_PREFIX=1 \
  COMPILER=g++-13 RELEASE=1 LOCALIZE=0 BACKTRACE=0 PCH=0 ASTYLE=0 tests

./tests/release-local-back-cata_test \
  '[multiplayer][root_lifecycle],[multiplayer][session_directory],[multiplayer][scheduler]' \
  --rng-seed 0 --user-dir /tmp/cdda-mp-root-lifecycle-combined-final-4

./tests/release-local-back-cata_test '[multiplayer]' \
  --rng-seed 0 --user-dir /tmp/cdda-mp-root-lifecycle-full-final

make -j"$(nproc)" AUTO_BUILD_PREFIX=1 \
  COMPILER=g++-13 RELEASE=1 LOCALIZE=0 BACKTRACE=0 PCH=0 \
  SANITIZE=address,undefined \
  WARNINGS='-Wall -Wextra -Wno-error=array-bounds' tests
ASAN_OPTIONS='detect_leaks=1:detect_stack_use_after_return=1:halt_on_error=1:abort_on_error=1' \
UBSAN_OPTIONS='print_stacktrace=1:halt_on_error=1' \
./tests/release-local-back-sanitize-cata_test \
  '[multiplayer][root_lifecycle],[multiplayer][session_directory],[multiplayer][scheduler],[multiplayer][phase_adapter]' \
  --rng-seed 0 --user-dir /tmp/cdda-mp-root-lifecycle-sanitize-final

make \
  ASTYLE_BINARY="$HOME/.cache/cdda-tools/astyle-3.1-3build1/root/usr/bin/astyle" \
  astyle-check
git diff --check
```

结果：

- GCC 13 incremental release `tests` 构建成功。
- root lifecycle + directory + scheduler：36 cases / 1,257 assertions，全过。
- 完整 `[multiplayer]`：104 cases；102 passed + 2 个既有 `[!mayfail]` full-avatar move-swap 负面对照；
  6,150 assertions 中 6,147 passed + 3 expected failures，exit 0。
- 定向 ASan/UBSan/LSan：46 cases / 1,489 assertions，全过，无 sanitizer finding。
- AStyle 3.1 与 `git diff --check` 通过。
- Gate 1 未接入 production command/world loop，因此本批不把旧 PTY smoke 继承为 lifecycle 行为证据，也不重复
  process smoke。diff 未修改 wire/schema/external public ABI、平台 conditional、shared source list 或 Windows/
  Android-owned code；新增 lifecycle header 是 internal shared C++ API。按 Tier 1 不运行 MSVC、Android package 或
  emulator/device。

### 前序 authoritative-session source 的本地 Linux 证据

source `eb990c4ad9975915336f3acd65431b47d123e842` 执行过：

```bash
source build-scripts/activate-multiplayer-build-env.sh
./build-scripts/check-multiplayer-build-env.sh linux
make -j8 \
  COMPILER=g++-13 TILES=0 SOUND=0 RELEASE=1 LOCALIZE=0 \
  BACKTRACE=0 PCH=0 ASTYLE=0 tests cataclysm
g++-13 -std=c++17 -O2 -Wall -Wextra -Werror \
  -Isrc -isystem src/third-party \
  tools/multiplayer/headless_client_smoke.cpp \
  src/multiplayer_protocol.cpp src/multiplayer_transport.cpp \
  src/multiplayer_crypto.cpp -pthread \
  -o build/multiplayer-smoke/headless_client_smoke

./tests/cata_test '[multiplayer][session_directory]' --rng-seed 0 \
  --user-dir test_user_dir_session_directory_final5
./tests/cata_test '[multiplayer][server_lobby]' --rng-seed 0 \
  --user-dir test_user_dir_server_lobby_final5
./tests/cata_test '[multiplayer][transport]' --rng-seed 0 \
  --user-dir test_user_dir_transport_final4
./tests/cata_test '[multiplayer][dedicated_server]' --rng-seed 0 \
  --user-dir test_user_dir_server_final4
./tests/cata_test '[multiplayer][protocol]' --rng-seed 0 \
  --user-dir test_user_dir_protocol_final2
./tests/cata_test '[multiplayer][client]' --rng-seed 0 \
  --user-dir test_user_dir_client_final2
./tests/cata_test '[multiplayer][scheduler]' --rng-seed 0 \
  --user-dir test_user_dir_scheduler_final2
./tests/cata_test '[multiplayer]' --rng-seed 0 \
  --user-dir test_user_dir_multiplayer_final3

make -j8 AUTO_BUILD_PREFIX=1 \
  COMPILER=g++-13 RELEASE=1 LOCALIZE=0 BACKTRACE=0 PCH=0 \
  SANITIZE=address,undefined \
  WARNINGS='-Wall -Wextra -Wno-error=array-bounds' \
  tests release-local-back-sanitize-cataclysm
ASAN_OPTIONS='detect_leaks=1:detect_stack_use_after_return=1:halt_on_error=1:abort_on_error=1' \
UBSAN_OPTIONS='print_stacktrace=1:halt_on_error=1' \
./tests/release-local-back-sanitize-cata_test \
  '[multiplayer][session_directory],[multiplayer][server_lobby],[multiplayer][dedicated_server],[multiplayer][transport]' \
  --rng-seed 0 --user-dir /tmp/cdda-mp-session-confirmation-sanitize-final2

FLATC="$PWD/build/flatbuffers-host/flatc" \
  tools/multiplayer/protocol/generate.sh --check
make \
  ASTYLE_BINARY="$HOME/.cache/cdda-tools/astyle-3.1-3build1/root/usr/bin/astyle" \
  astyle-check
git diff --check
```

结果：

- GCC 13 release `tests`、`cataclysm` 和 headless smoke client 构建成功。
- focused：session directory `5/121`、lobby `8/2708`、transport `7/165`、dedicated server `5/334`、protocol
  `9/253`、client `12/764`、scheduler `7/422`，全部通过。
- 完整 `[multiplayer]`：80 cases，78 passed + 2 个既有 `[!mayfail]` full-avatar move-swap 负面对照；5,436
  assertions 中 5,433 passed + 3 expected failures，exit 0。
- 定向 ASan/UBSan/LSan：25 cases / 3,328 assertions，全过；修复 sanitizer 首次发现的 transport close
  heap-use-after-free 后无 ASan、UBSan、LSan 或 stack-use-after-return finding。
- generated-header check、AStyle 3.1 和 `git diff --check` 通过。

### 前序 authoritative-session source 的本地真实进程

同一 source 已运行真实 Linux headless client 和 native curses client smoke：

```bash
./cataclysm \
  --userdir build/multiplayer-session-process-smoke-final/user \
  --server build/multiplayer-session-process-smoke-final/server.json
build/multiplayer-smoke/headless_client_smoke \
  127.0.0.1 38195 \
  build/multiplayer-session-process-smoke-final/server-auth-token.txt

./cataclysm \
  --userdir build/multiplayer-session-ui-smoke-final/server-user \
  --server build/multiplayer-session-ui-smoke-final/server.json
python3 tools/multiplayer/network_client_ui_smoke.py \
  --client "$PWD/cataclysm" --backend-port 51395 \
  --token-file build/multiplayer-session-ui-smoke-final/server-auth-token.txt \
  --user-dir build/multiplayer-session-ui-smoke-final/client-user \
  --transcript build/multiplayer-session-ui-smoke-final/client.transcript \
  --event-log build/multiplayer-session-ui-smoke-final/client-events.txt
```

- headless smoke 通过 auth/scene/resync/wait、generation `1 -> 2 -> 3` resume、duplicate replay 和 sequence
  payload mismatch rejection；server JSONL 含 accepted/duplicate/save/shutdown。
- curses PTY smoke 通过 wait、forced disconnect、generation 2 resume、uncertain command exactly-once duplicate、
  east move 和 clean quit；command statuses 为 `0, 2, 0`。
- 这些进程结果验证当前 session-directory source；production scheduler/offline/dormant 尚未接入，不能继承为下一
  gate 的行为证据。

### Hosted current-source 与继承的 public-boundary evidence

- Gate 1 source `cbd19b48d652be735a3c83fe841d2d7c831aecba` 的 Linux-only run
  [`29392575820`](https://github.com/wsdx233/Cataclysm-DDA-Multiplayer/actions/runs/29392575820) 为 terminal
  `success`。Primary Linux job `87279094495` 实际构建 source，并通过 pinned generated-header、GCC 13 production
  multiplayer tests、真实 headless server command/resume smoke、native-client UI resume smoke、Clang 18 和 evidence
  upload；selector job `87279064284` 成功，Windows MSVC job `87279095121` 与 Android NDK job `87279094964` 精确
  `skipped`。这证明当前 internal Gate 1 source 的 Linux production 回归，不新增 Windows/Android 当前候选证据。
- Baseline run [`29385561653`](https://github.com/wsdx233/Cataclysm-DDA-Multiplayer/actions/runs/29385561653)
  为 terminal `success`。本次因 protocol minor `1`/schema/generated header 属 Tier 2 public boundary，selector 只运行
  Windows x64 MSVC actual-source package job `87258227728` 和 Android actual-source package job `87258227735`；
  二者均成功。它们只证明本次 changed protocol production source 的 MSVC/NDK/Gradle 编译边界，不新增 Windows UI
  或 Android lifecycle 结论，也不构成后续内部 `.cpp` 的日常前置。
- Linux production run
  [`29385561675`](https://github.com/wsdx233/Cataclysm-DDA-Multiplayer/actions/runs/29385561675)
  为 terminal `success`。Primary Linux job `87257984217` 实际构建 production source，并通过 generated-header、GCC/
  Clang transport、完整 `[multiplayer]`、headless process smoke 和 native-client PTY resume smoke；standalone
  Windows/Android spike jobs 精确 skipped。

## 已知限制和未完成项

- ADR-0010 继续待验证；Gate 1 lifecycle contract 已关闭，但 production owner、fatal/no-save 和 Linux PTY regression
  未关闭前不得接受。
- scheduler/adapter 没有 production caller，actual legacy bubble 未置于 world claim 后。
- disconnect/forced-wait/world/offline 时序已有纯 contract 与组合测试，但 production server 尚未按该顺序执行；root
  dormant/resume 尚未进入真实 loop。
- `multiplayer_dedicated_server::complete_graceful_disconnect()` 当前把 lobby 产生 `send_and_disconnect` action 视为
  completion，`execute_actions()` 又忽略 transport `send_and_disconnect()` 的实际 enqueue/fallback 结果；Gate 2 在
  调用 `record_graceful_completion_queued()` 前必须区分 ACK queued 与 fallback/early close。
- root admission recipe 绑定 lifecycle owner/version，但不是 directory commit receipt。Gate 2 owner 必须把 directory
  plan/commit、root recipe、scheduler effect、enqueue/publish/cleanup 和 lifecycle record 封装为不可交叉的私有事务；
  若需要跨边界传递，再由 directory 返回 opaque receipt，不能把 recipe 描述为提交证书。
- production fault 尚未区分 pre-side-effect save 与 post-side-effect fatal no-save；可能发生非幂等副作用时不得写新
  canonical save。
- `players.max = 1`；第二 client、双玩家 command cache/roster 和 production round-robin 尚未实现。
- `game::walk_move()` 仍使用固定 `game::u`；human collision、monster target/attack、death、field/scent/NPC、tether/
  group shift 和 player-state isolation 尚未关闭。
- safe-mode character rules/whitelist 等仍有全局状态；当前只关闭 active-avatar permission gate 的已知污染。
- save 仍是 canonical single-avatar generation；durable process-restart token/save generation 和 portable character
  属后续阶段。
- 当前远程动作只有 wait 和八方向平面 move；其他动作必须 typed unsupported，不能进入 blocking UI。
- scene 仍是 full snapshot；items、fields、vehicles、overlays、messages、sound、avatar replica/panels 和完整 visibility
  leak matrix 属后续门禁。
- 无嵌入式 TLS；loopback 默认、trusted-LAN 显式例外和外部 authenticated tunnel 政策保持不变。

## 下一门禁和首个动作

Gate 1 已关闭。Gate 2 的首个具体动作是审查并改造 production owner 与 graceful transport result：

```bash
rg -n 'do_turn_remote|execute_turn_player_action|process_legacy_single_player_bubble_turn|complete_graceful_disconnect|send_and_disconnect|record_graceful|claim_world|record_world_completed' \
  src/main.cpp src/do_turn.cpp src/multiplayer_server.* \
  src/multiplayer_selected_root_lifecycle.* src/multiplayer_turn_phase_adapter.*
sed -n '1090,1285p' src/main.cpp
sed -n '150,315p' src/multiplayer_server.cpp
sed -n '520,930p' src/do_turn.cpp
```

### Gate 1：selected-root lifecycle contract/API（已关闭）

- source `cbd19b48d652be735a3c83fe841d2d7c831aecba` 定义并测试独立 connection/barrier/runtime/server/departure
  state、owner-bound ticket/lifecycle-version-bound recipe、same-runtime reactivation、caller-reported graceful
  ACK-queued/early-close ordering、timeout/resume race、world exact-once permission 和 guard/save/fault gate。
- real directory/runtime/scheduler 组合覆盖 disconnect forced wait -> world -> offline -> dormant -> resume、terminal
  graceful release，以及 unpublished grace `+1` repair；pure 状态表补齐 boundary/world-processing departure、early
  close、cross-owner/stale work 和 duplicate completion。
- Linux focused/full/sanitizer 证据已记录；wire/schema/external public ABI/platform-owned source 未变，新增 header
  仅为 internal shared C++ API，因此未运行 Windows/Android。
- 该 gate 只证明 contract/API；在 Gate 2 完成前不得宣称 production dormant 已接通。

### Gate 2：production single-root lifecycle

- 由 dedicated owner 私有持有 scheduler/adapter，一次接通：
  `disconnect -> barrier grace -> forced wait -> terminal record -> actual claim/world -> runtime offline -> dormant`。
- 先把 transport/event/admission pump 从 `do_turn_remote()` action callback 移到 outer server loop；initial unbound 和
  dormant 必须能处理 auth/resume，而不先进入 `turn_begin`/`player_begin` 或创建 guard。
- resume 必须在同一稳定 runtime 上完成 exact generation admission 并恢复 active，然后才解除 dormant；dormant 不
  启动新 turn、不创建 guard、不执行 command。
- semantic command 和 authoritative wait 必须经 adapter；adapter 接管 action 后绕过 legacy
  `execute_turn_player_action()` 外层 bookkeeping，避免双重记录。优先新增 typed owned-remote-turn seam 与显式 world
  hook，不用 bool callback/hack 表示“bookkeeping 已完成”。
- owned-turn seam 必须有不可跳过的 pre-world/player-phase-complete hook；sleep、activity 或 zero moves 可能完全跳过
  action callback，owner 仍须把单 root terminalize 并与 lifecycle 记录一致，不能让 scheduler 停在 awaiting 后直接跑
  legacy world。scheduler 需要显式的“无 command 但 player phase 已完成”terminal transition，不能伪造 accepted
  command result。
- actual `process_legacy_single_player_bubble_turn()` 必须位于不可绕过的 claim/record 路径。
- world hook 内完成 scheduler claim -> legacy bubble -> scheduler world completion；但 lifecycle 的 turn-boundary
  completion record 必须等 owned turn 的 player-end 全部完成并返回后才写。scene publish、interval/final save、runtime
  offline 和下一 turn 都必须排在该 record 之后。
- graceful ACK 只能在当前 barrier/world/offline 边界完成后进入 ordered completion。
- 修改 dedicated-server completion API，使它只有在 transport 明确返回 queued 时才记录 ACK queued；queue rejection、
  fallback close、peer/read/write failure 与 ACK 写完后的 ordered close 必须保留为不同 transport result，只有观察到
  exact terminal close event 后才调用 `record_graceful_transport_closed()`，不能共用一个 `completed` bool。
- directory commit、root admission recipe、scheduler/runtime/transport effect 和 lifecycle record 必须在 owner 内串行且
  不可交叉。任何外部 effect 已成功而匹配 record 不是 applied/duplicate 时立即 `latch_fault()`。
- execution fault 映射为 typed fatal shutdown；只要 non-idempotent gameplay side effect 可能已发生，就停止 simulation
  且不写新的 canonical save。只有证明为 pre-side-effect 的 failure 才允许正常保存。
- SIGINT/stop 也必须查询 lifecycle save disposition。当前 remote loop 可在 action wait 返回 null 后保存一个已经执行
  turn-begin/player-begin、尚未执行 world 的 partial turn；Gate 2 最小规则是 open turn 一律不走 normal save，只有
  clean connected boundary 或 dormant safe point 才允许写 canonical save。
- owner integration tests 补齐 dormant-origin unpublished retry 的真实全链和 grace 内正常 published `+1` 的真实
  directory + scheduler + lifecycle 全链，并注入 external-effect-success/record-failure、ACK queue rejection 和
  early-close races。
- owned-turn focused tests 覆盖 activity/sleep/zero-moves 跳过 action callback、bookkeeping 恰好一次、legacy world
  thunk 恰好一次、player-end 后才 lifecycle complete，以及 signal mid-turn 不保存。
- 编辑循环运行 Linux focused tests；gate 收口运行完整 `[multiplayer]`、定向 sanitizer、真实 Linux headless server +
  native client PTY，并记录 fatal/no-save fault injection。内部 source 不变更公共边界时不运行 Windows/Android。

### Gate 3：two-runtime owner 与规则矩阵

- 先把已绿色的 in-process two-runtime wait-only case 提升为 owner-level integration test，但继续拒绝
  `players.max > 1` 的外部配置。
- 再按可独立回归的批次关闭：round-robin move、human collision、monster/death、field/scent/NPC、tether/group
  shift、safe-mode/message/stats/player-state isolation。
- 规则门禁满足后先开放仅供 integration/process test 使用的双连接 routing，完成 Linux 双 client smoke/soak；该测试
  开关不能作为用户配置发布，也不表示 `players.max > 1` 已受支持。
- 每个规则批次使用 Linux focused tests；改变 shared turn/authority invariant 的批次在收口时补完整
  `[multiplayer]`，涉及生命周期/所有权时补 sanitizer，进入真实双 client routing 后补 Linux process smoke。
- owner/rule gates 与测试用双连接 smoke/soak 绿色后，才评估允许用户配置 `players.max > 1`。Phase 3 exit 前执行
  候选 commit/diff audit，复核哪些既有 Windows/Android 证据可继承，且只为受影响的平台或公共边界补必要 gate。

不得把当前单客户端 process smoke、in-process wait-only case 或 hosted protocol package 描述为两玩家 shared
barrier、完整 remote avatar replica、portable character 或 production release 完成。
