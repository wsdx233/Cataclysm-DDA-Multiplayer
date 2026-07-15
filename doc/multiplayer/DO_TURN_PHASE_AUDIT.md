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

这仍不是 ADR-0002 的多人 scheduler：当前只有一个 server-owned active avatar，一次完整 world phase 只由该
玩家的 action 推进。Phase 3 必须以现有 registry/guard 扩展逐玩家阶段，而不是复制 `do_turn_impl()`。

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
- production `src/main.cpp` 已用 authoritative directory binding 替换 `active_remote_session`，但仍只路由固定 root
  player，command replay cache 也只按 sequence 建索引；`do_turn_remote()` 会让这个 avatar 用尽 moves 后才进入一次
  world phase。它不是 round-robin 或多-player scheduler owner。
- `game::walk_move()` 仍直接使用固定 `game::u`，human-human collision、monster/death/field/scent/NPC target、
  tether/group-centered shift 和 player-scoped message/state 隔离都不是纯调度器能够补齐的能力。

复核没有推翻 ADR-0002，但证明五个 trace range 不能直接作为实现边界。当前 source 已先提取保持单人行为的
action/world seams 并增加 test-only phase adapter；production session/scheduler routing 尚未完成，在此之前保持
`players.max = 1`。

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

这些 API 的保证必须严格限定：纯 `record_automatic_wait_executed()` 无法证明真实 wait 已执行；下面的 source adapter
已经在 test-created runtime 上固定 execute-before-record，但尚未成为 production 唯一入口。world ticket 也只证明
scheduler 的 claim/record 状态转换；它不证明真实 bubble/world callback exactly-once。policy/adapter 都尚未接入
dedicated server，因此不能据此启用 `players.max > 1` 或宣称 Phase 3 gate 完成。

## Phase 3 phase adapter 与单人 seams

第二个 source slice 已增加 `multiplayer_turn_phase_adapter`、显式 wait execution mode 和两个保持单人顺序的窄入口：

- `game::record_turn_player_action( avatar & )` 负责 accepted action 的 `moves_since_last_save`/`action_taken()`；legacy
  `execute_turn_player_action()` 继续调用它。adapter 在目标 player guard 尚未退出时执行该 bookkeeping，避免 beta
  action 错误更新 root/alpha avatar。
- `game::process_legacy_single_player_bubble_turn( avatar &, map & )` 命名了原有 world block，但
  `do_turn_impl()` 仍直接调用它，随后才运行原有 `u.process_turn()`。这只是 extraction seam，不是 scheduler world
  claim 集成。
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
fail-stop、post-side-effect scheduler-record failure、world lambda one-shot，以及两个 registry runtime 的 wait-only
barrier。单远程 first-turn 回归还验证
`turn_begin -> player_begin -> action -> player_input -> world -> player_end` 顺序和 player-end moves replenishment。

边界仍必须准确描述：`rg` 只在 adapter 单测中找到其调用，production `main.cpp`/`do_turn_remote()` 没有 scheduler
owner；实际 legacy bubble 没有处于 `claim_world()` 后；world 测试只是 lambda；进程级 save/shutdown recovery、统一
session/runtime generation 和离线 runtime 激活策略都未实现。下一步先建立 authoritative session/runtime directory，
把 lobby auth/resume 改成 pending request -> simulation commit -> complete response，并解决单玩家 active root 无法
offline 的 lifecycle gate；再接入单玩家 production routing 和 actual claimed bubble，最后才扩展 production
two-runtime barrier。

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
| `monmove()` / `monster::plan()` / `monster::move()` | motion alarm、attitude、target、visibility、消息大量使用活动 getter | 从所有合法 human/NPC 目标选取；伤害和 RNG 执行一次；消息按观察者过滤 | Phase 3 两玩家 target/attack/visibility 矩阵 |
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
