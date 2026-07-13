# `game::do_turn()` 阶段基线与活动玩家审计

本文记录 Phase 0 对单人 `game::do_turn()` 的阶段观测边界和 world phase 中活动玩家依赖。
它是迁移清单，不表示当前循环已经支持多人，也不改变 ADR-0002 的共享 turn barrier 决策。

## 阶段观测边界

`multiplayer_turn_phase_trace` 在未安装 observer 时不读取时钟、不分配内存，只执行固定数量的空检查。
安装线程局部 observer 后，每段结束时报告 `steady_clock` 纳秒时长。当前标记顺序固定为：

| 阶段 | 当前代码范围 | 目标归属 |
| --- | --- | --- |
| `turn_begin` | calendar、weather 起始、timed events、item wakeups、missions | 每个共享 turn 恰好一次 |
| `player_begin` | 当前 avatar 的 vehicle/mount/body/activity、附近 NPC sound marker | Phase 3 对每名 barrier 玩家执行 |
| `player_input` | 本地输入循环或单远程玩家 semantic-command callback、activity continuation | Phase 2 已共享 wait/move executor；Phase 3 改为多玩家 barrier queue |
| `world` | scent、map falling/vehicle/field/item、explosion、monster/NPC、overmap NPC | 每个共享 turn 恰好一次，不得隐式代表单一玩家 |
| `player_end` | moves/body/weather/morale、可见性和本地音效收尾 | 规则部分逐玩家；UI/音效部分仅客户端 |

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
玩家的 action 推进。Phase 3 必须以 registry/guard 扩展逐玩家阶段，而不是复制 `do_turn_impl()`。

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

1. `turn_begin`、`world` 在一个 canonical turn 中只能各执行一次，不能放进 player guard 循环。
2. 逐玩家阶段必须通过 registry 的稳定 owner 创建 `multiplayer_active_player_guard`，不能切换固定 `game::u`
   的对象值。
3. world phase 不得让“最后一个活动玩家”隐式决定 monster target、NPC anchor、scent 或可见消息。
4. 先在 Phase 1 隔离 headless/UI 与 command queue，再在 Phase 3 拆 scheduler；Phase 0 不改 gameplay
   执行次序来伪装完成多人循环。
5. 每次移动边界时保留 phase-order 测试，并增加“每 turn 调用次数”断言；性能采样不得成为发布构建的
   无条件时钟开销。
