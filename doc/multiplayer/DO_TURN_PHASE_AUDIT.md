# `game::do_turn()` 阶段基线与活动玩家审计

本文记录 Phase 0 对单人 `game::do_turn()` 的阶段观测边界，以及 Phase 3 对这些范围、现有玩家
registry/context 和 world ownership 的复核。它是迁移清单，不表示当前循环已经支持多人，也不改变 ADR-0002
的共享 turn barrier 决策。

## 阶段观测边界

`multiplayer_turn_phase_trace` 在未安装 observer 时不读取时钟、不分配内存，只执行固定数量的空检查。
安装线程局部 observer 后，每段结束时报告 `steady_clock` 纳秒时长。**这些 label 只是 profiling/test 的
观测点，不是可直接交给 scheduler 的所有权边界。** 当前每个范围仍混合 player-scoped、world-once 和本地 UI
工作；迁移时必须先抽出明确 callback，不能把 label 包围的整段机械地放进逐玩家循环。当前标记顺序固定为：

| 阶段 | 当前代码范围 | 目标拆分提示（不是 label ownership） |
| --- | --- | --- |
| `turn_begin` | calendar、weather 起始、timed events、item wakeups、missions | 明确 global 子集每个共享 turn 恰好一次 |
| `player_begin` | 当前 avatar 的 vehicle/mount/body/activity、附近 NPC sound marker | 先抽离混入的 world work，再让 player-scoped 子集逐玩家执行 |
| `player_input` | 本地输入循环或单远程玩家 semantic-command callback、activity continuation | 抽出 command/activity callback；falling/cleanup/explosion 等另定 ownership |
| `world` | scent、map falling/vehicle/field/item、explosion、monster/NPC、overmap NPC | 提取 bubble/world callback，每共享 turn 恰好一次且不得隐式代表单一玩家 |
| `player_end` | moves/body/weather/morale、可见性和本地音效收尾 | 规则子集逐玩家；共享 weather/cache 与客户端 UI/音效分别抽离 |

正常 `do_turn()` 的测试必须按上述顺序各报告一次。游戏结束的 cleanup 提前返回不进入该序列；
行动中死亡等提前返回由 RAII trace 报告已经进入的阶段，不伪造尚未运行的阶段。

## Phase 1/2 单远程玩家路径

`game::do_turn()` 与 `game::do_turn_remote()` 现在共同进入私有 `do_turn_impl()`；前者保留原本本地行为，
后者只在 `player_input` 阶段调用 simulation-thread callback。callback 的 `std::optional<bool>` 合约为：

- `std::nullopt`：signal 或 runtime failure 要求中止等待；
- `false`：当前轮询没有产生 action；
- `true`：语义命令已执行并消费本轮玩家 action。

remote 路径不执行 renderer recovery、music/SFX、autosave、截图、blocking activity key polling、progress UI、
`FORCE_REDRAW` 或本地 game-over cleanup。dedicated runtime 还在动画入口统一抑制 explosion、bullet 和 hit
绘制；popup/debug/loading UI 有 headless suppression。`[multiplayer][command_executor]` 中的真实 turn 测试
证明 wait command 可由 callback 消费且不进入本地 input handler；实际进程 smoke 进一步证明无 curses/SDL
初始化即可完成 auth、scene、wait、world phase、save 和 signal shutdown。

这是 Phase 1/2 的 legacy callback 说明。Phase 3 production server 已改用下述 owned-turn/scheduler path，但仍只有
一个 server-owned active avatar；Gate 3 必须以现有 registry/guard 扩展逐玩家阶段，而不是复制 `do_turn_impl()`。

## Phase 3 source/ownership 复核

2026-07-14 的有序源码复核确认了以下事实：

- Phase 0 已经实现地址稳定的 `multiplayer_player_registry`、`multiplayer_player_runtime` 和仅模拟线程使用的
  `multiplayer_active_player_guard`，tracker/query/map-shift 正向矩阵已有测试。Phase 3 需要把它们接入 production
  session/scheduler；不应再次“新建”另一套 registry 或 guard。
- `player_begin` 不只是逐玩家准备。它同时包含 `overmap_buffer.process_mongroups()`/`move_hordes()`、weather
  更新、随机 NPC、light cache invalidation 等 world-once 工作，以及 vehicle/mount/body/activity/sound marker 等
  player-scoped 工作。
- `player_input` 也不是纯 command callback。每次输入轮询前还会运行 falling、dead cleanup、explosion、NPC/player
  sound marker 和 visibility/UI 更新；把整段轮流执行会重复世界副作用。
- `world` 当前以单一 `u` 写 scent source、用其位置作为 `scent.update()` 中心，并只调用
  `m.creature_in_field( u )`。`scent_map` 只有一份 `typescent`，不能在没有明确多 source 语义时简单循环两名玩家。
- `monmove()`、monster target/attack、NPC/overmap movement、可见消息和 group/map center 仍包含单活动 avatar
  假设；不能让“最后进入 guard 的玩家”隐式成为 world owner。
- `player_end` 同时包含 `u.process_turn()`（当前在这里补充下一轮 moves）、逐玩家 body/morale/power/weather effect
  和本地 renderer/SFX。ADR 的“turn begin 准备 moves”是目标模型；在 phase adapter 证明等价前，不应顺手改变
  当前 replenishment 次序并引入 off-by-one。
- production `src/main.cpp` 仍只路由固定 root player，但已由 `multiplayer_single_root_owner` 编排 directory、scheduler、
  lifecycle 和 adapter；command replay/cache 与 completed-scene revision 仍属于该单 root session。它不是
  round-robin multi-player owner。
- 本次审计发现 `game::walk_move()` 直接使用固定 `game::u`。Gate 3 source `3204f8f45606a20ea6ab0892369b9806f89c4353` 后续只把已审计的
  plain-walk 直接链改为读取 `active_avatar()`；source `84d8ca056bf72ed9776890f7ca4212a3bfdf7c2e` 又在该链之前关闭 adjacent semantic
  move 的 human-human occupied destination。source `6fe9be5ce8a6cca137b04b08002184c8ec8b0eb6` 进一步关闭
  4A.1 ordinary hostile monster target/basic melee 和 exact target-keyed `LOCKS_ON`。复杂/forced movement、
  monster direct specials/last-known-invisible/per-target attitude-hostility/messages-SFX、death/game-over、
  field/scent/NPC、tether/group-centered shift 和 player-scoped message/state 隔离仍不是纯调度器能够补齐的能力。

复核没有推翻 ADR-0002，但证明五个 trace range 不能直接作为实现边界。Gate 2 已把保持单人行为的 owned action/
world seams 和 phase adapter 接入 production single-root routing；五个 label 仍不能机械变成逐玩家 ownership。
production two-runtime outer owner 与上述规则缺口未关闭前保持 `players.max = 1`。

## Phase 3 纯 scheduler policy 切片

当前 Phase 3 source slice 的 `multiplayer_turn_scheduler` 是 backend-neutral 的纯策略/测试切片，不持有 live
avatar、socket、command payload 或 world callback。它已经收口以下调度语义：

- 每 turn 复制最多四人的 immutable roster snapshot；中途加入者延后到下一 turn。
- roster 使用稳定 `player_id` 顺序，并以“上个完成 turn 首位的字典序后继”轮换下一首位，对 roster churn 仍公平。
- ordering key 包含 shared turn、round、slot 和 `player_id`；current slot 在无命令时保持稳定。
- typed `accepted_remains_eligible`/`accepted_finished` 才推进；`rejected` 和 `duplicate` 不推进。
- session generation 是 barrier transition 的校验元数据；resume 需要精确旧 generation 且只允许 `+1`。
- timeout 只作用于 current disconnected slot。自动 wait 或移出 barrier 都先进入
  `automatic_wait_pending`，由调用方执行权威 wait 后再调用 `record_automatic_wait_executed()`。
- 所有参与者 terminal 后才进入 `world_ready`；`claim_world()` 转到 `world_processing` 并返回执行当前 turn world
  phase 的 permission marker，由 `record_world_completed()` 记录匹配 turn 后才回到 idle。

这些 API 的保证必须严格限定：纯 `record_automatic_wait_executed()` 无法证明真实 wait 已执行，world ticket 单独也
不能证明 actual bubble callback exactly-once。Gate 2 的 single-root owner 已把 adapter execute-before-record 和
one-shot actual world thunk 组合为 production 唯一入口；这仍不能据此启用 `players.max > 1` 或宣称 Phase 3 完成。

## Phase 3 phase adapter、owned turn 与单人 seams

第二个 source slice 已增加 `multiplayer_turn_phase_adapter`、显式 wait execution mode 和两个保持单人顺序的窄入口：

- `game::record_turn_player_action( avatar & )` 负责 accepted action 的 `moves_since_last_save`/`action_taken()`；legacy
  `execute_turn_player_action()` 继续调用它。owned remote path 则由 adapter 在目标 guard 尚未退出时执行该
  bookkeeping，并绕过 legacy 外层记录，避免重复或错误更新 root/alpha avatar。
- `game::do_turn_remote_owned()` 使用 typed hooks/result/completion token。mandatory player-phase-complete hook 在
  sleep/activity/zero-moves 跳过 action callback 时仍把 scheduler terminalize；公开字段不能伪造或重放 player-end
  completion proof。
- `game::process_legacy_single_player_bubble_turn( avatar &, map & )` 命名原有 world block；owned path 只能通过
  `multiplayer_owned_world_thunk` 在 exact scheduler world claim 中调用一次。ordinary single-player path 继续保持
  legacy direct order。
- player/wait entry 验证模拟线程、exact scheduler slot/state、player ID、session generation、runtime ownership/
  lifecycle、构造时 root context、guard engaged 和退出恢复；forced wait 绕过 safe-mode permission，但仍执行真实
  `Character::pause()`，成功后才 record scheduler transition。
- `check_safe_mode_allowed()` 的 laser lock、属性/trait、vehicle control、可见怪物、距离和方向都改为读取 active
  avatar，使 secondary player guard 的 permission gate 不再受固定 host avatar 状态污染；这不代表
  `get_safemode()` character rules、`lastmon_whitelist` 等全局 singleton 已完成玩家隔离。
- callback、wait、world 或 post-side-effect scheduler record failure 会 latch adapter 与 scheduler fault；scheduler
  保留当前 stage/participant 供诊断，但拒绝 replacement adapter、新 turn 和其他 transition。

Linux in-process tests 已覆盖 root-context precondition/正常恢复、active-avatar safe-mode permission、
stale/missing/inactive runtime、真实 forced pause before record、两种 disconnect policy 都先 wait、callback/exception
fail-stop、post-side-effect scheduler-record failure、actual world thunk one-shot、zero-action terminal transition、
player-end proof，以及两个 registry runtime 的 wait-only barrier。单远程回归保留
`turn_begin -> player_begin -> action -> player_input -> world -> player_end` 顺序和 player-end moves replenishment。

Gate 2 source `2e9236c7bf91782ad3f15daa5d8aa0e3929f355a` 已把 adapter、actual claimed bubble、exact directory generation、
offline/dormant reactivation 和 process-level save/shutdown recovery 接入 production single-root server。真实 Linux
headless 和 curses PTY 覆盖 auth/resume/duplicate/fresh command，open-turn SIGTERM 证明 fatal/no-save 且 save 哈希不变。
当前独立 multi-runtime inner owner 已由下述 source slice 建立；下一步是规则矩阵。不能通过把 single-root owner 的
`maximum_players == 1` 检查删除来跳过这些门禁。

## Gate 3 首个 multi-runtime barrier owner inner contract

Gate 3 source `2eccb92087991966423c18b63f6ef707462b1428` 新增
`multiplayer_multi_runtime_barrier_owner`，把原先直接组合 scheduler/adapter 的 two-runtime wait-only case 提升到
owner-level contract。它不是新的 production session owner，也没有把五个 trace label 提升为 ownership boundary：

- owner 只能在 simulation thread 构造和推进，容量为 2 至 4，但一个 turn 的 immutable roster 可以是 1 至 capacity
  个当前在线 participant。`begin_turn()` 在 scheduler mutation 前验证整个 roster 的唯一 player ID、exact generation、
  active lifecycle 和 registry ownership，并 pin 每个地址稳定 runtime 及其 avatar shared owner，直到外层 player-end
  completion 被精确接受。
- 每次 exact-current semantic action、zero-action terminal、authoritative forced wait 和 world claim 前，owner 都从
  registry 重新解析 runtime，核对 pinned/runtime/avatar owner 的裸地址与 shared control block、player identity、
  generation、active status 和 registry ownership。非 current 或 stale expected command 在 callback 前拒绝；若
  exact-current identity 已失效则 fail-stop，而不是把命令误路由给 replacement runtime。
- 一个持久 scheduler 跨 turn 保留公平首位轮换；每个 open barrier 只有一个 adapter。owner-level cases 让两个 registry
  runtime 在连续 turn 中分别执行真实 wait 和 disconnect timeout 后的 forced wait，并检查每次 action bookkeeping 恰好
  一次、active runtime/avatar 指向目标玩家且 callback 返回后恢复原 root context。
- scheduler world 成功后 owner 进入额外的 `player_end_pending`，继续 pin roster/runtime owners，并阻止第二次 world 或
  下一 turn。opaque completion receipt 绑定 owner、shared turn 和 epoch；exact receipt 应用一次，重复 exact receipt 为
  duplicate 仅在 owner 完全 idle 时成立；上一 turn receipt 在新 active turn 中返回 invalid 且不推进，新 pending
  boundary 上的 stale、default/forged 或 cross-owner receipt 会 latch external-player-end fault。
- callback、forced wait、world、context/identity recheck 或外层 completion record 失败会永久 latch fault；owner 记录
  gameplay/world side effect 是否可能已经发生。该标志只供未来 outer lifecycle/save owner 作 fail-stop 与 no-save
  判断，inner owner 自身不宣称 canonical state 或存档安全。

本 slice 的 world callback 仍是测试 lambda，不是 actual `process_legacy_single_player_bubble_turn()`。它还没有接入
production transport/session directory，也没有实现 multi-runtime resume/replay/repair、selected-root re-selection、
逐玩家 begin/end、outer lifecycle commit、scene publish 或 canonical save proof。production `do_turn()` 仍由
single-root owner 驱动 actual bubble，`players.max` 必须保持 `1`；因此该 inner contract 不表示 two-player routing、
Gate 3 关闭或 Phase 3 完成。下述第二个 inner rule slice 关闭 basic wait/move isolation；后续仍需在 outer
multi-runtime owner 中组合这些边界，再按 collision、monster/death、field/scent/NPC、tether/group shift 和
messages/safe-mode/stats/player-scoped cache isolation 的顺序关闭规则矩阵。

## Gate 3 第二个 command-router/basic-move inner rule contract

Gate 3 source `3204f8f45606a20ea6ab0892369b9806f89c4353` 新增 thin `multiplayer_multi_runtime_command_router`，把 exact-current inner owner
entry 与既有 `multiplayer_execute_basic_command()` 组合起来，但不接管 transport、admission、revision、dedup、scene
或 canonical save：

- owner 先完成 participant/generation/pinned runtime identity 与 root-context 验证，再在目标 active-player guard 内
  调用 router callback。non-current command 在 executor 前拒绝，router 不自行重选 runtime。
- accepted execution 根据目标 avatar 执行后的 moves 映射为 `accepted_remains_eligible` 或
  `accepted_finished`；无 action 的 rejected/duplicate 不推进 scheduler。status/action 不一致时不猜测 disposition，
  而是沿 adapter callback-failure path fail-stop。
- move 只允许已审计的同层相邻空地子集。occupied creature、vehicle/mount/grab/activity、field/trap/furniture/item、
  water/ramp/rough/sharp/unstable、小通道、door/open-air 和其他特殊 branch 在 gameplay 前拒绝。
- `game::walk_move()`、`get_dangerous_tile()`、`grabbed_move()` 和 `on_move_effects()` 的直接玩家读取改为
  `active_avatar()`。`avatar_action::move()` 继续向 walk seam 传递 `allow_interactive_ui`，因此 remote move 遇到危险
  terrain 时不会进入 prompt/ledge UI。该改动只证明直接 plain-walk 链，不代表完整 movement tree 已迁移。
- owner-level case 真实执行 exact `first move -> second move -> first wait -> second wait`，验证每次只有目标 position、
  moves、tracker index、`nv_cached` 和 action bookkeeping 改变；另一 avatar、root grab sentinel、runtime ownership 与
  guard 外 root context 保持。non-current、malformed、grabbed 和 human-occupied move rejected 且不推进 cursor。
- 所有 participant terminal 后 world callback 仍只是一次测试 lambda，随后 exact process-local player-end receipt 才
  返回 idle。它不是 actual legacy bubble 或 production two-client evidence。

production 仍由 single-root owner 路由，`players.max` 保持 `1`。下述第三个 inner rule slice 只关闭 adjacent
semantic-command path 的 human-human occupied destination；它不使 legacy `walk_move()`/`place_player()` 成为全局
collision boundary。

## Gate 3 第三个 human-collision/order-authority inner rule contract

源码审计确认 `avatar_action::move()` 的 generic creature test 会把 human 标记为 attacking，但后续只有 monster 和 NPC
typed branches；若只删除 router 的 creature preflight，另一 avatar 会落入 `walk_move()`/`place_player()`。后两者没有
Character occupancy authority，且 legacy movement 在 setpos 前已经可能改变 moves、stamina、facing、activity、noise、
cache 或进入 UI/monster-displacement 分支。因此 human collision 必须在 legacy callback 前裁决，而不能靠移动后的
overlap repair。

Gate 3 source `84d8ca056bf72ed9776890f7ca4212a3bfdf7c2e` 增加以下窄 contract：

- `multiplayer_player_registry::find_other_at()` 从 exact absolute-position index 排除 mover，再解析 registry-owned
  blocker runtime；它不把 generic creature order 或 position bucket 的首元素当作唯一 human identity。
- router 返回 internal typed `blocked_by_player` 与 exact blocker participant key；execution 是
  `rejected/invalid_state`、零 moves、零 action，scheduler disposition 是 `rejected`。adapter 因此不调用
  `record_turn_player_action()`，current slot、root context 和 owner no-side-effect state 保持。
- symmetric case 证明 root/secondary 双向 repeated block 不改变双方 position/moves/tracker/runtime/cache/bookkeeping，
  随后 wait 仍能完成 barrier。contested empty-tile case 证明 ordering-first success、second block，以及下一 turn 的
  first-player rotation 同时轮换 winner。
- blocker key 只供 inner authority；未来 outer result 必须 visibility-filter。该 slice 不提供 implicit swap/PvP、
  production two-client route、actual bubble、scene publish 或 canonical save proof；world 仍是 count lambda 加 exact
  player-end receipt。

因此五个 trace label 仍不是 ownership boundary，`walk_move()`/`place_player()` 也不是已批准的全局 human collision
guard。teleport、knockback、fling、vehicle/phasing 等 forced movement 必须在其自己的 authority slice 中验证或拒绝。
4A.2a.1 candidate-specific instantaneous attitude 已关闭；当前下一门禁是 ADR-0012 的 global/target-keyed
provocation scope 决策。之后依次审计 explicit-target specials、gun target lock、projectile actual hit、
last-known-invisible exact identity、fixed-root specials 和 forced/interactive/targetless-AoE policy。
observer-filtered messages/SFX 仍由后续 dedicated isolation 处理。随后的 4B 单独处理 `game::is_game_over()`
的 fixed `u/uquit`、blocking death UI 和 global cleanup。不能让最后一个 active-player guard 隐式决定 world
target 或终止整个 server。

## Gate 3 4A.1 ordinary monster target/basic-melee rule seam

Gate 3 source `6fe9be5ce8a6cca137b04b08002184c8ec8b0eb6` 将 world-phase 中最小的 ordinary hostile
monster/human seam 从 fixed active avatar 改为 explicit registry authority：

- runtime 只在 avatar 存活且 lifecycle 为 `active` 或 `offline` 时参与 world rules；`importing`、
  runtime-dead 和 avatar-dead 不是 target/occupant；
- `monster::plan()` 显式遍历 living humans，用目标自身 visibility 和几何 LoS 评估候选，对 equal
  rating 公平选择；当前 active-player guard 不能替换 world target population；
- `attack_target()`、`attack_at()` 和 `move()` 解析 exact living avatar，basic melee/stumble 只作用于
  selected target，不再回落到 fixed root；
- `LOCKS_ON` 保存 exact target `character_id` source，direct LoS 丢失后不自刷新、不转移到其他
  avatar；Tindalos teleport 只使用当前 target 的 exact lock。source-less 旧 effect 只在严格单 living-avatar
  情况保留兼容。

该 source 是 generic gameplay seam，Linux native binary 已编译链接，`--version` CLI path 正常；single-avatar
gameplay 与所有 two-runtime 断言都只由 in-process tests 覆盖。它没有 outer multi-runtime owner、actual
bubble/two-client route、scene publish 或 save proof，不允许提高 `players.max`，也不代表整个 4A、Gate 3 或
Phase 3 完成。
direct specials、last-known-invisible 和 provocation persistence 留给后续 4A.2；observer-filtered messages/SFX
留给 dedicated isolation；death/game-over lifecycle/save boundary 留给 4B。

## Gate 3 4A.2a.1 candidate-specific human attitude seam

Gate 3 source `53a81955f54f5517e58f5ab1b2e92c76d0ff034b` 把 ordinary planner 的可见 human observation 与
actionable target selection 分成两个显式步骤：

- 从所有 visible living humans 中按 rating/公平 tie 选择一个 observer，shared anger/morale triggers 每次
  `plan()` 只执行一次；observer 不保证是最终攻击目标，也不代表已聚合所有 humans 的 trigger 条件；
- trigger 完成后重新计算每个 exact candidate 的 `attitude()`/`is_fleeing()`；flee threat class 优先于普通
  `MATT_ATTACK` class，同类内再使用 rating/公平 tie；
- `KEEP_DISTANCE` 在多 living-avatar 下读取候选位置而不是旧 `get_dest()`；最终 Character target 重新计算自己的
  flee disposition，后续 monster target replacement 清除 human-specific disposition；
- 多 living-avatar basic `attack_at()` 对非 HOSTILE living avatar 在 moves、HP 和 action side effect 前返回 false；
  single living-avatar 路径保持 legacy attack routing；unique multi candidate case 不额外消费 RNG。

该 seam 没有修改 monster-wide serialized `aggro_character`。A 挑衅是否授权攻击 B 由待决策 ADR-0012 阻塞；
`living_world_avatar_count() > 1` 的分支在只剩一个存活玩家时回退 legacy，必须在 4B death/one-survivor policy 重审。
direct specials、gun lock、projectile actual hit、last-known-invisible、fixed-root hardcoded attacks、forced movement、
interactive/targetless/AoE、messages/SFX 均未关闭。generic source 链接进 Linux native binary，但 multi-runtime behavior
仍为 in-process `test-only`，没有 outer owner、actual bubble、wire route 或 save proof。

## Phase 0 基线

验证命令：

```bash
source build-scripts/activate-multiplayer-build-env.sh
make -j8 \
  COMPILER=g++-13 TILES=0 SOUND=0 RELEASE=1 LOCALIZE=0 \
  BACKTRACE=0 PCH=0 ASTYLE=0 tests
./tests/cata_test '[turn_phase]' -s \
  --user-dir test_user_dir_multiplayer_turn_phase_profile_clean --rng-seed 0
```

空地图、无输入、禁用 autosave 的单次 GCC 13 release 样本为：

| 阶段 | 纳秒 | 约合 |
| --- | ---: | ---: |
| `turn_begin` | 48,105 | 0.048 ms |
| `player_begin` | 16,757 | 0.017 ms |
| `player_input` | 10,797 | 0.011 ms |
| `world` | 3,338,876 | 3.339 ms |
| `player_end` | 130,984 | 0.131 ms |

这些数值只证明 observer 能分段并提供后续比较基线。它们不是性能预算，也不能代表有怪物、车辆、
大规模 field 或四名玩家的服务器负载。后续 benchmark 必须记录地图/实体数量、编译配置和样本分布。

## 审计方法

全源码静态入口扫描：

```bash
rg -n 'get_avatar\(\)|get_player_character\(\)|get_player_view\(\)' src
```

当前共有 1,354 处匹配，分布在 159 个文件；其中大部分不在 world phase。下面只列出
`do_turn()` 共享阶段的直接入口及其可达高风险语义，避免把文本匹配数误当作调用图覆盖率。

## 高风险依赖

| 入口 | 当前单玩家假设 | 多人所需语义 | 迁移门禁 |
| --- | --- | --- | --- |
| `timed_event_manager::process()` | `timed_event.cpp` 的事件效果可直接取活动 avatar | 全局事件执行一次；定向效果携带 `player_id` 或显式目标集合 | Phase 1 禁 UI；Phase 3 双玩家事件测试 |
| `mission::process_all()` | mission 回调和 active mission 列表可直接取活动 avatar | world mission 与玩家 mission 分作用域；玩家回调显式目标 | Phase 3 mission 隔离与冲突测试 |
| `scent.set/update()` | 只写当前 avatar scent，并以其位置更新 | 每名玩家 scent source 写入一次，共享 scent map 只推进一次 | Phase 3 双玩家 scent 测试 |
| `map::creature_in_field( u )` | 只处理当前 avatar 所在 field | 对所有 registry human player 各处理一次，map field 自身只推进一次 | Phase 3 field 伤害/移动冲突测试 |
| `map::vehmove()` | vehicle 路径中的 driver、ownership、可见消息可能取活动玩家 | driver 显式来自 vehicle；结果事件按可见玩家分发 | Phase 3 identity；Phase 6 完整驾驶 |
| `map::process_fields()` | `map_field.cpp` 的角色效果和 EOC dialogue 可取活动 avatar | 受影响实体显式传入；server rule path 禁止 popup/dialogue UI 栈 | Phase 1 headless guard；Phase 3 field 测试 |
| `sounds::process_sounds()` | AI sound 与当前玩家听觉/室内状态混用 | 世界 sound propagation 一次；每玩家可听事件和 marker 单独投影 | Phase 3 AI；Phase 4 per-player sound events |
| `monmove()` / `monster::plan()` / `monster::move()` | motion alarm、attitude、target、visibility、消息大量使用活动 getter | 从所有合法 human/NPC 目标选取；伤害和 RNG 执行一次；消息按观察者过滤 | 4A.1 已关 ordinary hostile target/basic melee/target-keyed lock；4A.2a.1 已关 candidate-specific instantaneous attitude；ADR-0012/provocation、specials、last-known invisible 与 messages/SFX 仍开放 |
| NPC turn loop | ally、enemy、talk、follow、可见性常以活动玩家为唯一 host | relation/target 使用稳定 ID；阻塞对话拆为 command/response state | Phase 1 禁 UI；Phase 3 AI；Phase 6 对话 |
| `overmap_npc_move()` | “near player”与 reload 距离以活动 avatar 为中心 | v1 使用共享 bubble/group anchor，不按最后激活玩家漂移 | Phase 3 tether 与 group-centered shift |
| `mon_info_update()`、visibility cache | 当前 player view 是唯一观察者 | world cache 与每玩家 visibility projection 分离 | Phase 4 visibility leak test |
| weather/body/morale/audio 收尾 | 规则更新与本地 UI/声音在同一尾段 | body/morale 对每玩家；renderer、popup、SDL sound 不进入 server | Phase 1 headless；Phase 3 player-end matrix |

## 不变量与后续顺序

1. `turn_begin`、bubble/world callback 在一个 canonical turn 中只能各执行一次，不能把当前 trace range 原样放进
   player guard 循环。
2. 逐玩家阶段必须通过 registry 的稳定 owner 创建 `multiplayer_active_player_guard`，不能切换固定 `game::u`
   的对象值；直接使用 `game::u` 的规则入口必须先适配或拒绝。
3. 自动 wait 必须在 forced/scoped adapter 内先成功执行再记录；world claim 后必须执行并计数真实 callback，
   同时定义失败恢复，不能把 policy 状态当作 gameplay 完成证据。
4. world phase 不得让“最后一个活动玩家”隐式决定 monster target、NPC anchor、scent、group shift 或可见消息。
5. 每次移动边界时保留 phase-order 测试，并增加“每 turn 真实 callback 调用次数”和失败路径断言；性能采样不得
   成为发布构建的无条件时钟开销。
6. adjacent semantic move 的 human block 必须在 legacy movement side effects 前完成；该 router guarantee 不能外推到
   forced movement。4A.1 ordinary monster target/basic melee 必须显式遍历 living registry-owned humans，且
   `LOCKS_ON` 必须绑定 exact target；4A.2a.1 的 candidate-specific attitude/basic guard 不能外推到 target-keyed
   provocation、direct specials、last-known-invisible、messages/SFX 或 death lifecycle。4B 仍必须定义逐玩家
   lifecycle，不能把 `game::u` 或 global `uquit` 当作多人 authority。
