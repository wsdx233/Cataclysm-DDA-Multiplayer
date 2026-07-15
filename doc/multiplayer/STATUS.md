# CDDA 多人 fork 当前状态

- 更新日期：2026-07-15
- 分支：`multiplayer/main`
- 当前 implementation source：`84d8ca056bf72ed9776890f7ca4212a3bfdf7c2e`
- 上游基线：`d84b90dd2aee090ca28c8dad5cdf1fab6dea151a`
- 当前阶段：**Phase 3，第二玩家/shared scheduler**
- 当前 active gate：**Gate 3，two-runtime owner 与共享规则矩阵**
- 已关闭 gate：Gate 1 selected-root lifecycle contract；Gate 2 production single-root lifecycle
- Gate 3 已关闭 slice：slice 1 inner barrier owner contract；slice 2 thin command router/basic move isolation；
  slice 3 typed human collision/order authority
- ADR 状态：ADR-0001 至 ADR-0011 均已接受
- 配置约束：`players.max = 1`；Gate 3 owner/rule/process gates 关闭前不得提高
- 当前验证策略：**Tier 1 Linux-first + production reachability**；internal/test-only slice 不强制 process smoke，
  baseline 手工入口默认 `target=linux`，只有完成的关键平台/公共/发布批次才升级对应平台

## 当前结论

Phase 3 Gate 2 已由 source `2e9236c7bf91782ad3f15daa5d8aa0e3929f355a` 关闭。production single-player
dedicated server 现在由 `multiplayer_single_root_owner` 在 simulation thread 私有持有 authoritative session
directory、turn scheduler、selected-root lifecycle 和 phase adapter：

- auth/resume 继续使用两阶段 admission 和 exact generation owner；admission plan、directory commit、transport
  publish receipt、unpublished cleanup、scheduler/runtime effect 与 lifecycle record 属于同一个 owner transaction；
- semantic wait/move 与 disconnect forced wait 经 adapter/active-player guard 执行，action bookkeeping 恰好一次；
- `game::do_turn_remote_owned()` 提供 mandatory player-phase completion，sleep/activity/zero-moves 不能绕过 terminal
  transition；
- actual `process_legacy_single_player_bubble_turn()` 只能经 exact world claim 的 one-shot thunk 执行；scheduler world
  completion 位于 world hook 内，lifecycle completion 等 player-end 完成并返回后才记录；
- root disconnect 超过 grace 后按 `forced wait -> terminal -> world -> player-end -> exact offline -> dormant` 推进；
  dormant 不开始 turn、不创建 guard、不执行 command，resume 先 re-activate 同一稳定 runtime；
- ADR-0011 的 outer pump 处理 initial/dormant/between-turn admission；只有 open player-input、world-before-claim 的
  同步 wait 可运行 bounded active-turn pump；world/player-end 不泵 control；
- open-turn resume/resync 只重放上一次 immutable completed-scene payload。command result 使用最近 completed scene
  revision；world + player-end + lifecycle completion 后才推进 revision、发布 scene、保存或开始下一 turn；
- graceful disconnect 明确区分 ACK `queued`、fallback close、stale request 和 fatal result；connection 一旦决定关闭，
  同批或已排队 semantic command 会在 gameplay 前被丢弃；
- owner 的 safe-boundary disposition 控制 shutdown/save。open turn 或 post-side-effect canonical state 不可证明时
  typed fatal/no-save；clean completed boundary 或 dormant 才允许 canonical save。

Gate 3 的首个 inner owner-contract slice 已由 source
`2eccb92087991966423c18b63f6ef707462b1428` 收口：

- 新增 `multiplayer_multi_runtime_barrier_owner`，容量固定为 2 至 4，但每 turn immutable roster 可包含 1 至 capacity
  个 exact active runtime；整个 roster 在 scheduler mutation 前完成 player ID/generation/registry ownership preflight；
- owner pin runtime/avatar shared owners，并在 exact action、zero-action terminal、forced wait 和 world 前复核同一 raw
  pointer 与 shared control block，same-key replacement、runtime loss 或 root-context mismatch 都在 callback/world 前
  fail-stop；
- 一个 persistent scheduler 跨 turn 保留公平首位轮换；two-runtime owner test 执行真实 semantic wait 与 disconnect
  automatic forced wait，并验证每次 bookkeeping 恰好一次及 root avatar/runtime 恢复；
- scheduler world 成功后进入显式 `player_end_pending`。process-local opaque receipt 绑定 owner/shared-turn/epoch，exact
  receipt 只应用一次，duplicate 只在 owner 完全 idle 时成立；旧 receipt 在新 active turn 中不能推进；
- callback/world/outer record divergence 永久 latch fault，并保留 gameplay side-effect uncertainty。exact receipt 只证明
  caller-reported player-end 顺序，不证明 outer lifecycle 或 canonical save safety。

Gate 3 的第二个 inner rule slice 由 source `3204f8f45606a20ea6ab0892369b9806f89c4353` 收口：

- 新增 thin `multiplayer_multi_runtime_command_router`。它不拥有 transport、admission、revision 或 dedup，只在
  `multiplayer_multi_runtime_barrier_owner` 已验证 exact current participant 后调用既有
  `multiplayer_execute_basic_command()`；
- router 只接受一致的 authoritative execution/disposition 组合：accepted action 根据目标 avatar 执行后的 moves
  映射为 `accepted_remains_eligible` 或 `accepted_finished`，无 action 的 rejected/duplicate 不推进 cursor；不一致
  组合返回无 disposition 并沿既有 adapter callback-failure path fail-stop；
- multi-runtime move 暂时 fail-closed 为已审计的同层相邻空地子集。vehicle/mount/grab/activity、non-human creature occupied、
  field/trap/furniture/item、water/ramp/rough/sharp/unstable、小通道、door/open-air 和其他 legacy 特殊分支都在规则
  执行前拒绝；
- `game::walk_move()`、`get_dangerous_tile()`、`grabbed_move()` 和 `on_move_effects()` 的直接玩家读取改为
  `active_avatar()`；`avatar_action::move()` 把 `allow_interactive_ui` 传入 walk seam，remote path 遇到危险地形不进入
  `query_yn()`/ledge UI；
- owner-level case 以 exact `first move -> second move -> first wait -> second wait` 顺序执行真实共享 executor，验证
  只有目标 position、moves、tracker index、`nv_cached` 和全局 action counter 改变；另一 avatar、root grab sentinel、
  runtime owner 与 root active context 保持不变；
- non-current、缺失 direction 和 grabbed move 均 rejected 且不改变 position/moves/bookkeeping、slot 或 fault state。
  slice 2 的 generic human-occupied rejection 已由下述 typed collision contract 取代。world 仍只执行测试 lambda，
  再由 exact player-end receipt 回到 idle。

Gate 3 的第三个 inner rule slice 由 source `84d8ca056bf72ed9776890f7ca4212a3bfdf7c2e` 收口：

- `multiplayer_player_registry::find_other_at()` 在 exact absolute position 查询除 mover 以外的 registry-owned human，
  不依赖 generic creature lookup 或 position bucket 的首元素恰好是谁；
- router 在调用 legacy `avatar_action::move()`/`walk_move()` 前产生 internal typed
  `multiplayer_multi_runtime_command_resolution::blocked_by_player`，并附 blocker 的 exact
  `{ player_id, session_generation }` participant key。该 key 只属于 inner authority；未来 outer wire owner 必须先做
  visibility filtering，不能原样泄露 generation 或不可见玩家身份；
- authoritative block 映射为 `status=rejected`、`rejection=invalid_state`、`moves_spent=0`、
  `action_taken=false` 和 scheduler `rejected`。它不交换位置、不触发 PvP/melee、不记录 action bookkeeping、不推进
  current slot，也不把 owner 标成发生了 gameplay side effect；
- symmetric owner case 让 root/secondary 双方各重复撞向相邻玩家两次，验证 exact blocker、slot、position、moves、
  tracker/runtime identity、`nv_cached`、global action counter、root context 和 fault/side-effect state 均不变；每人随后
  可提交 wait，barrier 正常进入 world 并以 exact player-end receipt 回到 idle；
- contested-tile case 在连续两个 shared turn 中让 ordering-first player 先占据同一空格，另一 player 随后得到 typed
  block。下一 turn 的 first-player rotation 令 winner 也精确轮换；每 turn 只执行一次 world count lambda，并保持
  `player_end_pending`、repeat-world-call rejection 和 exact completion 顺序。

这三个 Gate 3 slices 仍只是 inner barrier/rule contracts。production 继续使用 **single-root** owner；没有 actual
two-runtime bubble、session/resume owner、第二 client routing 或 save transaction。`players.max` 必须保持 `1`。

## 当前 ownership 快照

| 层 | 当前 owner/已完成边界 | Gate 3 缺口 |
| --- | --- | --- |
| Transport/lobby | connection、token mirror、pending admission、typed ordered close/rejection、closing priority | 双连接 admission/routing 仍未开放 |
| Session directory | stable identity/binding/generation、two-stage commit/confirmation、graceful pending、exact offline/reactivation | multi-runtime roster transition 尚未接入 production owner |
| Registry/runtime | 地址稳定 avatar/runtime、active/offline/dead、active-player guard、plain-move tracker identity、other-human exact-position lookup | monster/death、forced-movement collision 与复杂规则 selected-context |
| Scheduler | immutable roster、round-robin、disconnect grace、forced-wait pending、world ticket、fault latch | production two-runtime roster/routing；inner move/wait fairness 已验证 |
| Phase adapter | scoped action/wait/bookkeeping、world claim callback、post-effect fail-stop、blocked collision 不记 action | 第二 runtime 的 monster/death 与其余规则矩阵 |
| Multi-runtime barrier owner | immutable/pinned roster、persistent fairness、two-runtime wait/forced wait/plain move、explicit player-end pending receipt | 无 production caller、actual bubble、session/resume/root-selection/outer save owner |
| Multi-runtime command router | exact-current owner callback、shared basic executor、typed disposition、verified adjacent empty-ground move、typed human block/exact blocker/order conflict | 无 outer visibility/revision/dedup/transport/admission；monster/NPC 与复杂 movement 未关闭 |
| Single-root owner | directory/scheduler/lifecycle/adapter、dormant/resume、safe save disposition | 固定拒绝 `maximum_players != 1`，不得直接扩容 |
| Dedicated server | outer/active pump、completed-scene cache、typed fatal/no-save、真实 curses/headless loop | test-only 双连接开关和双 client smoke/soak 尚未实现 |

## 已关闭的 Gate 2 变化摘要

### Owned turn 与 scheduler

- 新增 `multiplayer_turn_scheduler::record_player_phase_completed()`，为无 action callback 的 player phase 提供显式
  terminal transition，不伪造 command result。
- 新增 typed `multiplayer_owned_remote_turn_hooks/result/token` 和 one-shot
  `multiplayer_owned_world_thunk`。completion token 绑定 owner/shared-turn/invocation，公开字段不能伪造或重放前一 turn
  的 player-end proof。
- adapter-owned action 绕过 legacy `execute_turn_player_action()` 外层 bookkeeping，避免双重记录。
- `can_execute_command()` 同时要求 lifecycle commandable 和 scheduler exact current slot `awaiting_command`。

### Production owner 与 lifecycle

- 新增 `src/multiplayer_single_root_owner.h/.cpp`；construction 明确要求 simulation thread 和
  `maximum_players == 1`。
- exact admission receipt、dormant-origin unpublished cleanup、same-generation replay、grace `+1` repair、graceful
  release pending、early close、forced wait/world/offline/dormant 和 same-runtime resume 均由 owner 组合。
- 任何 external effect 已成功但 matching lifecycle record 非 applied/duplicate 时立即 latch fault；若 gameplay/world
  side effect 可能已发生，则 canonical state 标记 uncertain 并拒绝新 save。

### Server loop、scene 与 close priority

- `run_dedicated_server()` 的 pump 已移出 legacy action callback。initial unbound/dormant 可 auth/resume 而不先进入
  turn；active-turn pump 只存在于 owned player-input wait。
- scene revision 表示 completed publish boundary。open barrier 的 resume/resync 换 exact session envelope 后重放缓存
  payload，不读取 partial live game state。
- lobby 在更新 inbound high-water/confirmation 前先完整验证 resync payload；main 在 authoritative confirmation 前
  校验 requested revision。
- `multiplayer_dedicated_server::connection_is_closing()` 跟踪所有 wrapper/lobby/transport/graceful/rejection close
  决策。合法 command 与 malformed control 同批到达时，合法 frame 可完成 tuple confirmation，但不会进入 scene/
  queue/gameplay；terminal disconnected event 消费后清除 closing state。

## Gate 3 首个 inner owner slice

- 新增 `src/multiplayer_multi_runtime_barrier_owner.h/.cpp`。该类只拥有一个 shared-turn barrier，不拥有 transport、
  directory、selected-root、逐玩家 begin/end、scene、save 或 production loop。
- `begin_turn()` 先验证整个 roster，再一次性保存 exact participant keys 和 runtime shared owners；invalid preflight 不
  消耗 shared-turn ID。per-turn roster 可少于 capacity，允许未来多人服务器只剩一个在线 runtime 时继续使用同一 owner。
- exact current action、zero-action terminal、automatic wait 与 world 全 roster 在任何规则回调前复核 pinned runtime/
  avatar owner identity、generation、active status 和 registry ownership。同 key replacement 不会把旧 barrier 的 action
  或 world 路由到新对象。
- owner 私有持有 persistent scheduler，每个 open barrier 只创建一个 adapter。两 turn 的真实 two-runtime wait case
  证明首位精确轮换、semantic/automatic wait 各 bookkeeping 一次、active-player guard 选择目标 runtime，并恢复 root。
- world success 只产生 process-local opaque completion receipt 并进入 `player_end_pending`；next turn、第二次 world 和
  roster pin release 都等待 exact external player-end record。receipt 不可持久化或跨 owner replacement 异步排队；若
  未来 outer owner 需要该能力，先改成独立 durable owner tag。
- 旧 adapter-level two-runtime wait-only case 已移除，等价且更强的 owner-level integration 现在是唯一证据位置。

## Gate 3 第二个 command-router/basic-move slice

- 新增 `src/multiplayer_multi_runtime_command_router.h/.cpp`。router 是 owner 与既有 semantic executor 之间的薄层；
  revision、client sequence dedup、transport connection、admission 和 scene publish 仍属于未来 outer owner。
- exact-current participant 由 barrier owner 建立 guard。router 不自行选择 runtime，也不绕过 owner 的 pinned owner
  identity 复核；non-current command 在 executor callback 前拒绝。
- wait 沿用 `multiplayer_execute_basic_command()`；move 只有通过 audited adjacent empty-ground preflight 才进入同一
  executor。合法执行结果再按目标 post-action moves 产生 scheduler disposition，rejected/duplicate 不消费当前 slot。
- plain-walk 直接链改为 active-avatar-aware，并显式禁止 remote dangerous-terrain prompt。这只批准已测试子集，不能
  推导 door、attack、grab、vehicle、phasing、swim、field/trap 或其他 legacy movement branch 已安全。
- `tests/multiplayer_multi_runtime_command_router_test.cpp` 覆盖 exact `move/move/wait/wait` round-robin、目标唯一
  position/moves/action bookkeeping、registry/tracker identity、root grab 隔离、root context 恢复，以及 non-current、
  malformed、grabbed 和 human-occupied destination 的 fail-closed rejection。
- world callback 仍是 owner-level count lambda；本 slice 没有 production caller，也不构成 actual bubble、two-client 或
  canonical save evidence。

## Gate 3 第三个 human-collision/order-authority slice

- `src/multiplayer_player_registry.h/.cpp` 新增 `find_other_at()`，从 exact absolute-position index 中排除 mover 后返回
  另一 registry-owned avatar。它不会把 `find_at().front()` 等同于“唯一 occupant”，也不把 generic creature order
  当作 human authority。
- `src/multiplayer_multi_runtime_command_router.h/.cpp` 把 routed result 分类为 `executor_result`、
  `unsupported_move` 或 `blocked_by_player`。human block 携带 exact blocker participant key；matching runtime 若无法
  从 registry 解析，callback 不猜测 disposition，而是沿现有 adapter fail-stop 边界处理。
- human collision 在 legacy action callback 前结束，所以不会进入 `move_effects()`、`walk_move()`、`place_player()`、
  NPC menu、monster attack、swap 或 movement UI/side effects。blocking execution 是合法格式但当前 authoritative state
  不允许的 command，因此用 `invalid_state`，而不是把它误写为 malformed command 或 accepted no-op。
- `multiplayer_multi_runtime_command_router_blocks_adjacent_active_players_symmetrically` 覆盖 root/secondary 两个方向、
  repeated block 不推进、随后 wait 可完成 barrier，以及 world/player-end exact closure。
- `multiplayer_multi_runtime_command_router_rotates_contested_tile_winner` 覆盖两个 player 争同一 empty tile、first success/
  second typed block、remaining-moves round、两 turn first/winner rotation、tracker uniqueness、每次 accepted action 唯一
  bookkeeping，以及每 turn world lambda/receipt exact-once。
- 本 slice 没有定义 swap 或 PvP；也没有审计 teleport、knockback、fling、vehicle、phasing 或其他绕过 router 的 forced
  movement。production 仍无 multi-runtime caller，blocker key 也尚未进入 visibility-filtered wire result。

## 验证策略与 CI 入口优化

workflow source `25347cc3d5538657e67790e3d7a83aaae25b796c` 把 baseline 手工默认 target/fallback 从 `all` 改为
`linux`；本次文档同时把 closure 规则改为 production reachability 驱动，并将当前 monster/death 工作拆成 4A/4B。
变更类别为 `workflow-control + documentation`，changed-source reachability 为 `test-only`；没有修改 game、protocol、
package content、platform-owned source、pinned toolchain 或 `players.max`。

本地验证：

```bash
tmp=$(mktemp -d)
base=https://github.com/rhysd/actionlint/releases/download/v1.7.12
curl -fsSL --retry 3 "$base/actionlint_1.7.12_linux_amd64.tar.gz" -o "$tmp/actionlint.tar.gz"
curl -fsSL --retry 3 "$base/actionlint_1.7.12_checksums.txt" -o "$tmp/checksums.txt"
expected=$(awk '$2 == "actionlint_1.7.12_linux_amd64.tar.gz" { print $1 }' "$tmp/checksums.txt")
printf '%s  %s\n' "$expected" "$tmp/actionlint.tar.gz" | sha256sum -c -
tar -xzf "$tmp/actionlint.tar.gz" -C "$tmp" actionlint
"$tmp/actionlint" .github/workflows/multiplayer-baseline.yml

selector=$(python3 - <<'PY'
from pathlib import Path
import yaml

doc = yaml.safe_load(Path( '.github/workflows/multiplayer-baseline.yml' ).read_text())
workflow_dispatch = doc.get( 'on', doc.get( True ) )['workflow_dispatch']
target = workflow_dispatch['inputs']['target']
assert target['default'] == 'linux'
assert target['options'] == ['linux', 'windows', 'android', 'all']
step = next( step for step in doc['jobs']['changes']['steps']
             if step.get( 'name' ) == 'Select package targets' )
assert "inputs.target || 'linux'" in step['env']['MANUAL_TARGET']
print( step['run'] )
PY
)
bash -n <<<"$selector"
for target in linux windows android all; do
  out=$(mktemp)
  run_tmp=$(mktemp -d)
  EVENT_NAME=workflow_dispatch MANUAL_TARGET="$target" \
    BASE_SHA= GITHUB_SHA=$(git rev-parse HEAD) \
    GITHUB_OUTPUT="$out" RUNNER_TEMP="$run_tmp" bash -c "$selector"
  case "$target" in
    linux) expected='linux=true windows=false android=false' ;;
    windows) expected='linux=false windows=true android=false' ;;
    android) expected='linux=false windows=false android=true' ;;
    all) expected='linux=true windows=true android=true' ;;
  esac
  test "$(tr '\n' ' ' < "$out" | sed 's/ $//')" = "$expected"
done

git diff --check
```

结果：actionlint/YAML parse/Bash syntax 全过；manual selector 精确输出 `linux=true`、`windows=true`、
`android=true` 的对应单平台组合，`all` 输出三者全 true；旧的全平台默认值和文档描述已无残留。未运行本地 game build、
Windows package 或 Android APK，因为该 control/docs 变更不修改它们的源码或产物；任何自动 hosted package run 只有
terminal 后才可作为 workflow/artifact 证据，不能外推为 Gate 3 gameplay 证据。现有 automatic selector 对 baseline
workflow 文件自身仍会 `select_all`，所以本 source 推送后可能有一次全 package run；后续 CI-cost slice 应把纯
dispatch/selector control 变更分流到轻量静态 gate，真正 package-job 变化仍保留全矩阵。

## 当前 source 验证证据

### Gate 3 slice 3 Linux release、完整多人 suite 与格式

```bash
source build-scripts/activate-multiplayer-build-env.sh
./build-scripts/check-multiplayer-build-env.sh linux

make -j"$(nproc)" AUTO_BUILD_PREFIX=1 \
  COMPILER=g++-13 RELEASE=1 LOCALIZE=0 BACKTRACE=0 PCH=0 ASTYLE=0 \
  tests

./tests/release-local-back-cata_test \
  '[multiplayer][multi_runtime_command_router]' \
  --rng-seed 0 --user-dir /tmp/cdda-mp-human-collision-focused-final2

./tests/release-local-back-cata_test '[multiplayer]' \
  --rng-seed 0 --user-dir /tmp/cdda-mp-human-collision-full-final2

make \
  ASTYLE_BINARY="$HOME/.cache/cdda-tools/astyle-3.1-3build1/root/usr/bin/astyle" \
  astyle-check
git diff --check
```

结果：

- focused router：4 cases / 817 assertions，全过；
- 完整 `[multiplayer]`：143 cases；141 passed + 2 个既有 `[!mayfail]` full-avatar move-swap identity 负面对照；
  8,353 assertions 中 8,350 passed + 3 expected failures；exit 0；
- AStyle 3.1 与 `git diff --check` 通过。

### Gate 3 slice 3 Linux sanitizer

```bash
source build-scripts/activate-multiplayer-build-env.sh
CXXFLAGS='-Wno-array-bounds -Wno-stringop-overread' \
  make -j"$(nproc)" AUTO_BUILD_PREFIX=1 \
  COMPILER=g++-13 RELEASE=1 LOCALIZE=0 BACKTRACE=0 PCH=0 ASTYLE=0 \
  SANITIZE=address,undefined tests

ASAN_OPTIONS='detect_leaks=1:detect_stack_use_after_return=1:halt_on_error=1:abort_on_error=1' \
UBSAN_OPTIONS='print_stacktrace=1:halt_on_error=1' \
./tests/release-local-back-sanitize-cata_test \
  '[multiplayer][multi_runtime_command_router]' \
  --rng-seed 0 --user-dir /tmp/cdda-mp-human-collision-sanitize-final2
```

结果：4 cases / 817 assertions，全过；无 ASan、UBSan、LSan 或 stack-use-after-return finding。沿用 slice 2 已记录的
两个 GCC 13 unchanged-source `-Werror` diagnostic suppressions；sanitizer flags 保持启用，这些 suppressions 不是
sanitizer finding。

本 slice 没有 production caller，且没有修改 single-root dedicated server/client route，因此不运行 Linux native-client
PTY 或 headless process smoke。该省略是按风险选择，不是 production two-runtime evidence。

### Gate 3 slice 2 Linux release、完整多人 suite 与格式

```bash
source build-scripts/activate-multiplayer-build-env.sh
./build-scripts/check-multiplayer-build-env.sh linux

make -j"$(nproc)" AUTO_BUILD_PREFIX=1 \
  COMPILER=g++-13 RELEASE=1 LOCALIZE=0 BACKTRACE=0 PCH=0 ASTYLE=0 \
  tests

./tests/release-local-back-cata_test \
  '[multiplayer][multi_runtime_command_router],[multiplayer][command_executor]' \
  --rng-seed 0 --user-dir /tmp/cdda-mp-gate3-router-command-focused-final

./tests/release-local-back-cata_test '[multiplayer]' \
  --rng-seed 0 --user-dir /tmp/cdda-mp-gate3-router-full

rm -f release-local-back-obj/version.o
make -j"$(nproc)" AUTO_BUILD_PREFIX=1 \
  COMPILER=g++-13 RELEASE=1 LOCALIZE=0 BACKTRACE=0 PCH=0 ASTYLE=0 \
  release-local-back-cataclysm
./release-local-back-cataclysm --version

make \
  ASTYLE_BINARY="$HOME/.cache/cdda-tools/astyle-3.1-3build1/root/usr/bin/astyle" \
  astyle-check
git diff --check
```

结果：

- router + command executor focused：7 cases / 225 assertions，全过；
- 完整 `[multiplayer]`：141 cases；139 passed + 2 个既有 `[!mayfail]` full-avatar move-swap identity 负面对照；
  7,736 assertions 中 7,733 passed + 3 expected failures；exit 0；
- source commit 后的 curses client/server binary 报告 build ID
  `3204f8f45606a20ea6ab0892369b9806f89c4353-dirty` 和 exact `-tiles, -sound`；`dirty` 仅来自本次未提交文档；
- AStyle 3.1 与 `git diff --check` 通过。

### Gate 3 slice 2 Linux sanitizer

```bash
source build-scripts/activate-multiplayer-build-env.sh
CXXFLAGS='-Wno-array-bounds -Wno-stringop-overread' \
  make -j"$(nproc)" AUTO_BUILD_PREFIX=1 \
  COMPILER=g++-13 RELEASE=1 LOCALIZE=0 BACKTRACE=0 PCH=0 ASTYLE=0 \
  SANITIZE=address,undefined tests

ASAN_OPTIONS='detect_leaks=1:detect_stack_use_after_return=1:halt_on_error=1:abort_on_error=1' \
UBSAN_OPTIONS='print_stacktrace=1:halt_on_error=1' \
./tests/release-local-back-sanitize-cata_test \
  '[multiplayer][multi_runtime_command_router],[multiplayer][command_executor],[multiplayer][multi_runtime_barrier_owner],[multiplayer][phase_adapter],[multiplayer][scheduler]' \
  --rng-seed 0 --user-dir /tmp/cdda-mp-gate3-router-sanitize-final
```

结果：47 cases / 1,568 assertions，全过；无 ASan、UBSan、LSan 或 stack-use-after-return finding。未加 suppressions
的首次全量重编译先被 unchanged `character_inventory.cpp` 的 GCC 13 `-Warray-bounds` 阻断；仅抑制该诊断后又被
unchanged `character_inventory.cpp`/`editmap.cpp` 的 `-Wstringop-overread` 阻断。上面的最终命令同时抑制这两个
`-Werror` diagnostics，sanitizer flags 仍保持启用；这不是 sanitizer finding，也没有关闭 sanitizer。

### Gate 3 slice 2 Linux native curses client PTY regression

配置根：`build/multiplayer-gate3-router-ui-20260715`，backend port `43235`。

```bash
set -euo pipefail
root="$PWD/build/multiplayer-gate3-router-ui-20260715"
port=43235
test ! -e "$root"
if ss -ltn | rg -q ":${port}\\b"; then
  echo "port $port already in use" >&2
  exit 99
fi
mkdir -p "$root"
./release-local-back-cataclysm \
  --userdir "$root/server-user" --init-server-config "$root/server.json" \
  >"$root/init.stdout" 2>"$root/init.stderr"
jq --arg listen "127.0.0.1:$port" \
  '.network.listen = $listen | .players.disconnect_grace_seconds = 3' \
  "$root/server.json" >"$root/server.json.tmp"
mv "$root/server.json.tmp" "$root/server.json"

stdbuf -oL -eL ./release-local-back-cataclysm \
  --userdir "$root/server-user" --server "$root/server.json" \
  >"$root/server.stdout" 2>"$root/server.stderr" &
server_pid=$!
cleanup() {
  if kill -0 "$server_pid" 2>/dev/null; then
    kill -TERM "$server_pid" 2>/dev/null || true
    wait "$server_pid" || true
  fi
}
trap cleanup EXIT
for _ in $(seq 1 600); do
  rg -q '"event":"listening"' "$root/server.stdout" && break
  kill -0 "$server_pid"
  sleep 0.1
done
rg -q '"event":"listening"' "$root/server.stdout"

python3 tools/multiplayer/network_client_ui_smoke.py \
  --client "$PWD/release-local-back-cataclysm" \
  --backend-port "$port" \
  --token-file "$root/server-auth-token.txt" \
  --user-dir "$root/client-user" \
  --transcript "$root/client.transcript" \
  --event-log "$root/client-events.txt" \
  --timeout-seconds 60 \
  >"$root/client.stdout" 2>"$root/client.stderr"
sleep 1
kill -TERM "$server_pid"
wait "$server_pid"
trap - EXIT

test "$(jq -r 'select(.event == "command_result") | .status' \
  "$root/server.stdout" | paste -sd, -)" = '0,2,0'
test "$(rg -c '"event":"player_resumed"' "$root/server.stdout")" -eq 1
rg -q 'sent local quit input' "$root/client.stdout"
rg -q 'client exited with status 0' "$root/client.stdout"
rg -q 'scene_sync_events=3' "$root/client.stdout"
rg -q '"event":"save_completed".*"revision":"4"' "$root/server.stdout"
rg -q '"event":"shutdown"' "$root/server.stdout"
! rg -q '"event":"save_refused"|"event":"runtime_failed"' "$root/server.stdout"
```

结果：command statuses `0,2,0`，`player_resumed = 1`，client exit `0`，`scene_sync_events = 3`；server 记录
`save_completed(revision=4)` 与 `shutdown`，无 `save_refused`/`runtime_failed`。这证明 unchanged production single-root
headless/native-client path 在 active-avatar walk seam 修改后仍正常；router 本身仍无 production caller，不能把本 PTY
描述为 two-runtime process evidence。

### Gate 3 Linux release、完整多人 suite 与格式

```bash
source build-scripts/activate-multiplayer-build-env.sh
./build-scripts/check-multiplayer-build-env.sh linux

make -j"$(nproc)" AUTO_BUILD_PREFIX=1 \
  COMPILER=g++-13 RELEASE=1 LOCALIZE=0 BACKTRACE=0 PCH=0 ASTYLE=0 \
  tests release-local-back-cataclysm

./tests/release-local-back-cata_test \
  '[multiplayer][multi_runtime_barrier_owner]' \
  --rng-seed 0 --user-dir /tmp/cdda-mp-gate3-owner-pinned-final

./tests/release-local-back-cata_test \
  '[multiplayer][multi_runtime_barrier_owner],[multiplayer][phase_adapter],[multiplayer][scheduler]' \
  --rng-seed 0 --user-dir /tmp/cdda-mp-gate3-barrier-owner-focused-final2

./tests/release-local-back-cata_test '[multiplayer]' \
  --rng-seed 0 --user-dir /tmp/cdda-mp-gate3-barrier-owner-full-final

./release-local-back-cataclysm --version
make \
  ASTYLE_BINARY="$HOME/.cache/cdda-tools/astyle-3.1-3build1/root/usr/bin/astyle" \
  astyle-check
git diff --check
```

结果：

- Linux environment gate 与 GCC 13 release source/tests、curses client/server binary 构建成功；client version smoke
  报告 exact `-tiles, -sound`。
- owner focused：14 cases / 379 assertions，全过。
- owner + phase adapter + scheduler：40 cases / 1,351 assertions，全过。
- 完整 `[multiplayer]`：138 cases；136 passed + 2 个既有 `[!mayfail]` full-avatar move-swap identity 负面对照；
  7,522 assertions 中 7,519 passed + 3 expected failures；exit 0。
- AStyle 3.1 与 `git diff --check` 通过。

### Gate 3 Linux sanitizer

```bash
source build-scripts/activate-multiplayer-build-env.sh
make -j"$(nproc)" AUTO_BUILD_PREFIX=1 \
  COMPILER=g++-13 RELEASE=1 LOCALIZE=0 BACKTRACE=0 PCH=0 ASTYLE=0 \
  SANITIZE=address,undefined tests

ASAN_OPTIONS='detect_leaks=1:detect_stack_use_after_return=1:halt_on_error=1:abort_on_error=1' \
UBSAN_OPTIONS='print_stacktrace=1:halt_on_error=1' \
./tests/release-local-back-sanitize-cata_test \
  '[multiplayer][multi_runtime_barrier_owner],[multiplayer][phase_adapter],[multiplayer][scheduler]' \
  --rng-seed 0 --user-dir /tmp/cdda-mp-gate3-barrier-owner-sanitize-final2
```

结果：40 cases / 1,351 assertions，全过；无 ASan、UBSan、LSan 或 stack-use-after-return finding。该 inner source
没有 production/network caller，因此本 slice 不增加 headless process smoke；真实双 client process gate 留到 test-only
routing 接通时。

### Gate 2 历史 Linux build、release tests 与格式

本批使用当前 workspace Linux toolchain；实现工作树在无后续 source 修改的情况下提交为
`2e9236c7bf91782ad3f15daa5d8aa0e3929f355a`。Gate 2 process/full-suite binary 在提交动作前生成，内嵌 build ID 因此
仍显示 `fb3ca50-dirty`；提交前 staged source diff 与所验证工作树一致，提交后没有再修改 source。source commit 后
以下 build 命令又完整成功，当前 binary build ID 为 `2e9236c7bf91782ad3f15daa5d8aa0e3929f355a-dirty`，其中
`dirty` 只来自本次文档收口：

```bash
source build-scripts/activate-multiplayer-build-env.sh
./build-scripts/check-multiplayer-build-env.sh linux

make -j"$(nproc)" AUTO_BUILD_PREFIX=1 \
  COMPILER=g++-13 RELEASE=1 LOCALIZE=0 BACKTRACE=0 PCH=0 ASTYLE=0 \
  tests release-local-back-cataclysm

./tests/release-local-back-cata_test \
  '[multiplayer][single_root_owner]' \
  --rng-seed 0 --user-dir /tmp/cdda-mp-gate2-owner-final

./tests/release-local-back-cata_test \
  '[multiplayer][phase_adapter],[multiplayer][scheduler]' \
  --rng-seed 0 --user-dir /tmp/cdda-mp-gate2-turn-scheduler-final

./tests/release-local-back-cata_test \
  '[multiplayer][dedicated_server],[multiplayer][server_lobby]' \
  --rng-seed 0 --user-dir /tmp/cdda-mp-gate2-server-final

./tests/release-local-back-cata_test '[multiplayer]' \
  --rng-seed 0 --user-dir /tmp/cdda-mp-gate2-full-final-20260715

make \
  ASTYLE_BINARY="$HOME/.cache/cdda-tools/astyle-3.1-3build1/root/usr/bin/astyle" \
  astyle-check
git diff --check
```

结果：

- Linux environment gate、GCC 13 release tests 和 production curses/server binary 构建成功。
- single-root owner：11 cases / 374 assertions，全过。
- phase adapter + scheduler：28 cases / 1,104 assertions，全过。
- dedicated server + lobby：16 cases / 3,549 assertions，全过。
- 完整 `[multiplayer]`：125 cases；123 passed + 2 个既有 `[!mayfail]` full-avatar move-swap identity 负面对照；
  7,174 assertions 中 7,171 passed + 3 expected failures；exit 0。
- AStyle 3.1 和 `git diff --check` 通过。

headless protocol client 另以 production sources 和 warning-as-error 构建：

```bash
g++-13 -std=c++17 -O2 -Wall -Wextra -Werror \
  -Isrc -isystem src/third-party \
  tools/multiplayer/headless_client_smoke.cpp \
  src/multiplayer_protocol.cpp src/multiplayer_transport.cpp \
  src/multiplayer_crypto.cpp -pthread \
  -o build/multiplayer-smoke/headless_client_smoke
```

结果：编译成功，无 warning。

### Gate 2 历史 Linux sanitizer

```bash
source build-scripts/activate-multiplayer-build-env.sh
ASAN_OPTIONS='detect_leaks=1:detect_stack_use_after_return=1:halt_on_error=1:abort_on_error=1' \
UBSAN_OPTIONS='print_stacktrace=1:halt_on_error=1' \
./tests/release-local-back-sanitize-cata_test \
  '[multiplayer][single_root_owner],[multiplayer][dedicated_server],[multiplayer][server_lobby]' \
  --rng-seed 0 --user-dir /tmp/cdda-mp-gate2-sanitize-final-source
```

结果：27 cases / 3,923 assertions，全过；无 ASan、UBSan、LSan 或 stack-use-after-return finding。此前更宽的 Gate 2
sanitizer batch 也覆盖 phase adapter/scheduler；最终 localized close-priority 和 owner-commandability 修复后，以上
affected final-source sanitizer 重新运行并绿色。

### Gate 2 历史 Linux headless process smoke

配置根：`build/multiplayer-gate2-headless-final2-20260715`，loopback port `43205`，
`disconnect_grace_seconds = 3`。

```bash
set -euo pipefail
root="$PWD/build/multiplayer-gate2-headless-final2-20260715"
port=43205
rm -rf "$root"
mkdir -p "$root"
./release-local-back-cataclysm \
  --userdir "$root/user" --init-server-config "$root/server.json" \
  >"$root/init.stdout" 2>"$root/init.stderr"
jq --arg listen "127.0.0.1:$port" \
  '.network.listen = $listen | .players.disconnect_grace_seconds = 3' \
  "$root/server.json" >"$root/server.json.tmp"
mv "$root/server.json.tmp" "$root/server.json"

stdbuf -oL -eL ./release-local-back-cataclysm \
  --userdir "$root/user" --server "$root/server.json" \
  >"$root/server.stdout" 2>"$root/server.stderr" &
server_pid=$!
for _ in $(seq 1 600); do
  rg -q '"event":"listening"' "$root/server.stdout" && break
  kill -0 "$server_pid"
  sleep 0.1
done
rg -q '"event":"listening"' "$root/server.stdout"

build/multiplayer-smoke/headless_client_smoke \
  127.0.0.1 "$port" "$root/server-auth-token.txt" \
  >"$root/client.stdout" 2>"$root/client.stderr"
sleep 5
kill -TERM "$server_pid"
wait "$server_pid"

test "$(rg -c '"event":"player_resumed"' "$root/server.stdout")" -eq 3
test "$(jq -r 'select(.event == "command_result") | .status' \
  "$root/server.stdout" | paste -sd, -)" = '0,2,0'
rg -q 'open-barrier cached resume/resync' "$root/client.stdout"
rg -q 'revisions 1 -> 2 -> 3' "$root/client.stdout"
rg -q '"event":"command_sequence_conflict"' "$root/server.stdout"
rg -q '"event":"save_completed".*"revision":"4"' "$root/server.stdout"
rg -q '"event":"shutdown"' "$root/server.stdout"
! rg -q '"event":"save_refused"|"event":"runtime_failed"' "$root/server.stdout"
```

结果：

- handshake rejection/acceptance、fresh auth、initial completed scene 和 resync 通过；
- zero-action open barrier raw close 后 generation 2 resume；resume scene 与上一次 completed/resync payload 逐字节一致；
  open barrier 再次 resync 仍逐字节一致；
- sequence 4 wait accepted，generation 3 resume 后 sequence 4 duplicate，再执行 sequence 5 fresh wait；
- generation 4 resume 后 changed-payload sequence-4 conflict 触发 terminal close；
- `player_resumed` 共 3 次；command statuses 为 `0,2,0`；completed revisions 为 `1 -> 2 -> 3`；
- 等待超过 grace 后 SIGTERM，server exit 0，记录 `save_completed(revision=4)` 和 `shutdown`；无
  `save_refused`/`runtime_failed`。

### Gate 2 历史 Linux native curses client PTY

配置根：`build/multiplayer-gate2-ui-final-20260715`，backend port `43215`。

```bash
set -euo pipefail
root="$PWD/build/multiplayer-gate2-ui-final-20260715"
port=43215
rm -rf "$root"
mkdir -p "$root"
./release-local-back-cataclysm \
  --userdir "$root/server-user" --init-server-config "$root/server.json" \
  >"$root/init.stdout" 2>"$root/init.stderr"
jq --arg listen "127.0.0.1:$port" \
  '.network.listen = $listen | .players.disconnect_grace_seconds = 3' \
  "$root/server.json" >"$root/server.json.tmp"
mv "$root/server.json.tmp" "$root/server.json"

stdbuf -oL -eL ./release-local-back-cataclysm \
  --userdir "$root/server-user" --server "$root/server.json" \
  >"$root/server.stdout" 2>"$root/server.stderr" &
server_pid=$!
for _ in $(seq 1 600); do
  rg -q '"event":"listening"' "$root/server.stdout" && break
  kill -0 "$server_pid"
  sleep 0.1
done
rg -q '"event":"listening"' "$root/server.stdout"

python3 tools/multiplayer/network_client_ui_smoke.py \
  --client "$PWD/release-local-back-cataclysm" \
  --backend-port "$port" \
  --token-file "$root/server-auth-token.txt" \
  --user-dir "$root/client-user" \
  --transcript "$root/client.transcript" \
  --event-log "$root/client-events.txt" \
  --timeout-seconds 60 \
  >"$root/client.stdout" 2>"$root/client.stderr"
sleep 1
kill -TERM "$server_pid"
wait "$server_pid"

test "$(jq -r 'select(.event == "command_result") | .status' \
  "$root/server.stdout" | paste -sd, -)" = '0,2,0'
test "$(rg -c '"event":"player_resumed"' "$root/server.stdout")" -eq 1
rg -q 'sent local quit input' "$root/client.stdout"
rg -q 'client exited with status 0' "$root/client.stdout"
rg -q 'scene_sync_events=3' "$root/client.stdout"
rg -q '"event":"save_completed"' "$root/server.stdout"
rg -q '"event":"shutdown"' "$root/server.stdout"
! rg -q '"event":"save_refused"|"event":"runtime_failed"' "$root/server.stdout"
```

结果：真实 curses client 完成 wait、proxy 强制断开、generation-2 resume、uncertain command duplicate、east move 和
local quit；command statuses `0,2,0`，scene sync events `3`，client/server 均 exit 0；server clean save/shutdown，无
fatal/no-save event。

### Gate 2 历史 open-turn fatal/no-save process gate

配置根：`build/multiplayer-gate2-open-turn-nosave-final-20260715`，loopback port `43225`。真实 curses client 在 private
PTY 连接后不输入 action；server 观察到 `player_authenticated` 且无 `command_result`，随后收到 SIGTERM。

```bash
set -euo pipefail
root="$PWD/build/multiplayer-gate2-open-turn-nosave-final-20260715"
port=43225
rm -rf "$root"
mkdir -p "$root"
./release-local-back-cataclysm \
  --userdir "$root/server-user" --init-server-config "$root/server.json" \
  >"$root/init.stdout" 2>"$root/init.stderr"
jq --arg listen "127.0.0.1:$port" \
  '.network.listen = $listen | .players.disconnect_grace_seconds = 3' \
  "$root/server.json" >"$root/server.json.tmp"
mv "$root/server.json.tmp" "$root/server.json"

stdbuf -oL -eL ./release-local-back-cataclysm \
  --userdir "$root/server-user" --server "$root/server.json" \
  >"$root/server.stdout" 2>"$root/server.stderr" &
server_pid=$!
for _ in $(seq 1 600); do
  rg -q '"event":"listening"' "$root/server.stdout" && break
  kill -0 "$server_pid"
  sleep 0.1
done
rg -q '"event":"listening"' "$root/server.stdout"

client_cmd="$PWD/release-local-back-cataclysm --userdir $root/client-user/"
client_cmd+=" --connect 127.0.0.1:$port"
client_cmd+=" --connect-token-file $root/server-auth-token.txt"
setsid script -q -e -c "$client_cmd" "$root/client.typescript" \
  </dev/null >"$root/client.stdout" 2>"$root/client.stderr" &
client_pid=$!
for _ in $(seq 1 600); do
  rg -q '"event":"player_authenticated"' "$root/server.stdout" && break
  kill -0 "$server_pid"
  sleep 0.1
done
rg -q '"event":"player_authenticated"' "$root/server.stdout"
sleep 0.25
! rg -q '"event":"command_result"' "$root/server.stdout"

find "$root/server-user/save/coop-world" -type f -print0 | \
  sort -z | xargs -0 sha256sum >"$root/save.before"
kill -TERM "$server_pid"
set +e
wait "$server_pid"
server_status=$?
set -e
kill -TERM -- "-$client_pid" 2>/dev/null || true
wait "$client_pid" || true
find "$root/server-user/save/coop-world" -type f -print0 | \
  sort -z | xargs -0 sha256sum >"$root/save.after"

test "$server_status" -eq 1
cmp "$root/save.before" "$root/save.after"
! rg -q '"event":"command_result"|"event":"save_completed"' "$root/server.stdout"
rg -q '"event":"save_refused".*"disposition":"2"' "$root/server.stderr"
rg -q '"event":"runtime_failed".*owned turn did not reach an exact player-end lifecycle boundary' \
  "$root/server.stderr"
```

结果：server exit `1`；记录 `save_refused` disposition `2` 和
`runtime_failed("owned turn did not reach an exact player-end lifecycle boundary")`；没有 `save_completed`；退出前后
`server-user/save/coop-world` 全部文件 SHA-256 清单完全一致。client 在 server half-close 后由 harness 清理，未产生
gameplay command。

## 最近关闭 slice 的平台判定

本批选择 Tier 1，因为 Gate 3 slice 3 只修改 backend-neutral internal registry/router policy 与 owner tests。收口后
使用下述 diff 审计：

```bash
git diff --name-status \
  c2ed0fc84680db3c100cfd95304ca723fa693059..84d8ca056bf72ed9776890f7ca4212a3bfdf7c2e
```

审计未发现 Android/Windows-owned source、platform conditional、wire schema/version/capability、generated protocol、
transport/crypto public boundary、shared source list、workflow、artifact 或 pinned toolchain 变化。registry/router header
新增的 lookup、resolution 和 blocker key 都是 repo-internal C++ authority seam，不改变 wire/public DTO/serialization ABI，
也没有平台分支。故本批不运行 Windows MSVC package、Android APK/NDK 或 emulator/device；这是按策略未运行，不是
blocker，也不构成 `84d8ca056bf72ed9776890f7ca4212a3bfdf7c2e` 的平台证据。后续若修改 platform conditional、公共
ABI/header、source list 或平台 owned code，必须重新分类并补第 20.6 节要求的最小 Tier 2 gate。

最近兼容的历史证据：

- Gate 1 source `cbd19b48d652be735a3c83fe841d2d7c831aecba` 的 Linux production run `29392575820` terminal
  `success`；Windows/Android jobs 精确 skipped。
- protocol minor `1` public-boundary source `eb990c4ad9975915336f3acd65431b47d123e842` 的 baseline run
  `29385561653` 对 Windows/Android actual production source 编译绿色。它只覆盖未变化的 protocol boundary，不证明
  当前 Gate 3 source 在这些平台编译或运行。

## 已知限制

- `players.max = 1`；现有 multi-runtime owner/router 只是 inner barrier/rule contract，没有 production outer session owner、第二
  client routing、actual two-runtime bubble 或 shared round-robin process evidence。
- multi-runtime owner 尚未组合 directory resume/replay/repair、selected-root re-selection、逐玩家 begin/end、scene
  publish 或 canonical save transaction。process-local completion receipt 只允许在 owner 存活期同步使用。
- production wait/move 都消耗完整 standard move budget。owner exact test 已覆盖
  `accepted_remains_eligible -> second command`；未来任何自然保留 moves 的 executor 启用前，必须增加真实 process
  回归。
- production game-over/death 当前进入 typed fatal/no-save；monster targeting、player death 和可证明的 death save
  boundary 属 Gate 3。
- adjacent semantic move 的 human occupied destination 已关闭为 typed authoritative block，但没有批准 implicit swap/PvP，
  也没有证明 teleport/knockback/fling/vehicle/phasing 或直接 `place_player()` 等 forced-movement path 的全局 collision
  invariant。monster/NPC attack、door/furniture/vehicle/grab/phasing/swim、field/trap/effect 等复杂 movement branch 仍未
  批准；monster/death、field/scent/NPC、tether/group shift、messages/safe-mode/stats/player-scoped cache isolation 未关闭。
- 当前远程动作只有 wait 和八方向平面 move；其他动作必须 typed unsupported，不能进入 blocking UI。
- scene 仍为 full snapshot；items、fields、vehicles、overlays、messages、sound、avatar replica/panels 和完整 visibility
  leak matrix 属 Phase 4/后续 gate。
- save 仍是 canonical single-avatar generation；durable process-restart resume、portable character 和 multiple save
  generations 属后续阶段。
- 无嵌入式 TLS；loopback 默认、trusted-LAN 显式例外和外部 authenticated tunnel 政策不变。

## 下一门禁与首个动作

当前 active gate 是 Gate 3。inner owner-contract、basic move/wait isolation 和 typed human collision/order authority
分别由 source `2eccb92087991966423c18b63f6ef707462b1428`、
`3204f8f45606a20ea6ab0892369b9806f89c4353` 和 `84d8ca056bf72ed9776890f7ca4212a3bfdf7c2e` 关闭；下一步仍不得修改 single-root owner 的
`maximum_players == 1` 约束。原 monster/death 大切片拆成 4A 和 4B，避免把 rule selection 与 lifecycle/save policy
混在一次实现和一次高成本验证中。

已完成的 source audit 结论：

- `monster::plan()` 仍先取固定 `get_player_character()`，hostile target/LOCKS_ON 只把 root avatar 放入候选；NPC 和
  monster 候选随后独立处理。`monster::attack_target()` 与 `melee_attack( Creature & )` 能沿 explicit target/tracker
  命中 secondary avatar，但 movement 使用的 `monster::attack_at()` 仍只特判固定 root，再查 monster/NPC；因此 4A
  必须同时关闭 authoritative human candidate selection 和 actual movement attack routing，不扩散到 message/SFX 隔离。
- `game::is_game_over()`、`turn_handler::cleanup_at_end()` 和全局 `uquit` 都是 single-avatar/global UI 语义，不能复用为
  单玩家死亡。4B 必须单独定义“一个玩家死亡但 world 继续”“全员死亡 terminal”及 runtime/save/fail-stop boundary。
- `multiplayer_player_runtime::mark_dead()` 已有状态转换，但当前 game wrapper 拒绝标记 active runtime；4B 在修改前要
  明确 transition owner 和 exact safe point，不能先用全局 game-over 流程拼接。

首个具体动作是 4A：确认 planner/`attack_at()` 的固定-root seam 和 registry 的稳定 runtime/avatar 枚举，再定义无
root bias 的 human candidate contract 与 actual attack route。首个命令：

```bash
sed -n '390,525p;1325,1415p;1935,2035p' src/monmove.cpp
rg -n 'attack_at\(|attack_target\(|melee_attack\(|monster::plan\(|rate_target\(|multiplayer_players\(\)' \
  src/monmove.cpp src/monster.cpp src/game.cpp \
  src/multiplayer_player_registry.* src/multiplayer_player_runtime.* tests/multiplayer_*
```

Gate 3 的 ordered slices：

1. **已关闭，source `2eccb92087991966423c18b63f6ef707462b1428`：** 独立 multi-runtime inner barrier owner 与
   owner-level two-runtime wait-only integration；外部配置继续拒绝 `players.max > 1`。
2. **已关闭，source `3204f8f45606a20ea6ab0892369b9806f89c4353`：** thin multi-runtime command router、verified adjacent empty-ground
   move，以及 round-robin move/move/wait/wait 的 position/moves/action-bookkeeping isolation。
3. **已关闭，source `84d8ca056bf72ed9776890f7ca4212a3bfdf7c2e`：** adjacent human occupied destination 的 typed
   `blocked_by_player`、exact blocker、symmetric block 与 contested-tile winner rotation；无 implicit swap/PvP。
4. **当前 4A：monster target/attack。** 明确 eligible human candidates、死亡/离线状态、visibility/tie selection、
   `attack_at()` movement routing 和 actual damaged avatar。Linux client build 覆盖 production source 编译，focused
   tests 覆盖 two-runtime 行为；后者 reachability 仍为 `test-only`。若改变 authority invariant 再加完整
   `[multiplayer]`，不为尚无 two-runtime production caller 的路径跑伪 process gate。
5. **4B：death/game-over safe boundary。** 覆盖单玩家死亡、仍有存活玩家、全员死亡、runtime transition 与
   save/fail-stop disposition。默认增加完整 `[multiplayer]` 和定向 sanitizer；只有接入 production shutdown/save 路径
   后才跑 Linux headless/native-client process regression。
6. field/scent/NPC。
7. tether/group shift。
8. messages/safe-mode/stats/player-scoped cache isolation。
9. 仅在上述 owner/rule gates 绿色后，增加 test-only 双连接 routing；运行 Linux headless server + 两个 Linux native
   clients smoke，再完成 60 分钟 soak；之后才评估开放
   `players.max > 1`。

后续 slice 按重构计划第 20.6 节记录 `test-only/client/server/end-to-end` reachability。Windows/Android 仅在冻结的
完成批次实际触及对应 owned code、公共 wire/ABI/toolchain 或平台产品声明时运行最小必要 gate；普通 Gate 3 internal
`.cpp`/tests 保持 Linux-only。
