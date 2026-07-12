# ADR-0005：稳定 avatar 地址与活动玩家上下文

- 状态：已接受
- 日期：2026-07-12
- 关联计划：第 6、7、9、20 节

## 背景

当前 `game::u`、`get_avatar()` 和 `get_player_character()` 表示唯一活动 avatar，调用点遍布玩法代码。`game::do_turn()` 和大量 action 还隐含“世界中只有一个 avatar”。一次性改成所有规则显式传入 `player_context` 会造成大范围侵入式修改，并显著增加长期合并上游的成本。

初始计划尝试把 `game::u` 当作固定执行槽，在槽与 registry 之间 move-swap 完整 `avatar`。Phase 0 普通测试证明 avatar 的序列化值可以完成 10,000 次 round-trip，但也确认了两个核心阻塞：持久 wielded `item_location` 会失效，character-backed `item_location` 的 cached carrier 会继续指向旧槽并取得另一玩家 ID。这说明“玩家身份随值移动、引用仍绑定对象地址”的模型不能按原样进入生产实现。

`Character::swap_character()` 加 avatar sidecar 仍会移动 weapon、inventory、worn、activity 等 Character 主体，因此 sidecar 本身只能解决状态分类，不能自动解决对象地址与引用身份问题。

## 决策

多人 registry 为每名玩家稳定分配一个 `player_runtime` 和完整 `avatar`。玩家在加入世界后，其 runtime、avatar 和 `character_id` 在在线/离线保留期内不得因容器扩容或活动玩家切换而移动。`game` 保存一个可重定向的活动玩家指针，现有全局 getter 返回该指针指向的 avatar。

规则执行通过 `active_player_guard` 激活目标玩家：

- guard 只能在模拟线程和明确安全点创建。
- guard 必须接收 registry/runtime 提供的 shared owner（runtime 内嵌 avatar 时使用 aliasing shared pointer），并在整个激活区间持有它；活动裸指针只能是 `shared_owner.get()` 的短期借用视图。
- 进入时只切换活动 avatar/runtime 指针，以及消息 sink、stats、achievements、safe mode 等玩家作用域上下文；不 move-swap avatar 或 Character 主体。
- 退出时恢复上一个上下文，并检查角色 ID、对象地址、位置、item UID、tracker 和 shared pointer 身份不变量。
- 非活动 human player 必须作为玩家实体参与占位、可见性、攻击和 creature 查询，但不能运行 NPC AI。
- 现有 `game::u` 在迁移期只作为单人默认 avatar 的 backing storage；多人路径不得把固定 `u` 槽等同于当前玩家。相关直接访问必须逐步改为活动 getter 或显式玩家参数。
- world/bubble phase 中不得依赖活动 getter 代表全部玩家；相关高风险调用要显式改为目标玩家、最近 human player 或所有玩家语义。

持久引用以稳定身份为事实来源：

- 跨 command safe point 的角色引用使用 `player_id`/`character_id` 和 session generation。
- 跨物品移动、activity step、save/load 的物品引用使用 `item_uid` 与 owner/location context。
- `Character *`、`avatar *`、`item *` 和 safe-reference cache 只能作为短期借用或可验证缓存；缓存身份不匹配时必须丢弃并按稳定 ID 重新解析。
- registry 使用 `unique_ptr`/shared ownership 或其他地址稳定容器，不直接把 avatar 值放入会重分配的 vector。

完整 avatar move-swap 保留为失败对照，不再作为原样生产候选。sidecar 用于承载消息、stats、safe mode 等 player-scoped 状态，但不作为移动 Character 主体的方案。在稳定地址门禁完成前，不把 Phase 0 guard/registry 描述为生产玩家执行环境，也不接入远程命令执行。

## 替代方案

- 完整 avatar move-swap：拒绝按当前形式采用。已用可重复测试证明 persistent item reference 和 carrier identity 失败。
- swap 后遍历并修复全部引用：拒绝。无法证明 activity、safe reference、vehicle、mount、mission、EOC 和第三方 mod 中所有缓存均被覆盖。
- `Character::swap_character()` 加 avatar sidecar：不作为地址问题的解决方案。它仍移动 Character 主体；sidecar 只保留 player-scoped 状态拆分价值。
- 立即把所有规则改为显式 `player_context` 参数：拒绝作为首个边界。优先让现有 getter 转发到稳定活动指针，再按 phase audit 逐步显式化。
- 把其他玩家永久建模为普通 NPC：拒绝。NPC AI、序列化、阵营和 avatar-only 状态语义均不正确。
- 每名玩家持有独立 `game`：拒绝。同一世界会产生多个 map、tracker、RNG 和 world phase。
- 让网络线程直接切换活动玩家并执行动作：拒绝。违反单模拟线程权威边界。

## 后果

- 在过渡期内，现有玩法代码仍可通过 `get_avatar()`/`get_player_character()` 访问当前执行者，减少首批改动。
- `game` 内直接使用固定成员 `u` 的路径必须由编译器和 phase audit 分批迁移；Phase 0 guard 不能被描述为已完成的多人执行环境。
- active guard 成为高风险边界，需要调试上下文、嵌套规则和异常/提前返回安全性。
- Phase 0 sidecar 已迁出消息、stats、achievements、safe-mode 和 per-player remote-vehicle cache；map
  memory、diary、recipes 及 avatar 本体中的 missions、bionics、mutations 已完成双玩家切换和 snapshot
  证据。后续仍须按 action/phase 审计分类其他自动化规则和纯客户端 UI 状态。
- Phase 0 POC 已把当前活动 runtime/avatar shared owner 与活动裸指针成对切换；registry 为额外 avatar 提供真实 shared ownership，并让 `shared_from()` 返回同一控制块。legacy `u_shared_ptr` 的 null-deleter alias 只保留给 `game` 自身拥有的单人 backing avatar。
- 稳定地址不等于持久引用永远有效；物品在 inventory、wielded、ground、vehicle 之间正常移动时仍需按 UID 重新验证。

## Phase 0 验证结果

- GCC 13 release/curses 正向测试对两个地址稳定 avatar 完成 10,000 次切换，覆盖 registry ownership、
  活动/非活动 human-player 查询、真实 shared control block、位置索引、inventory/worn/wielded/nested item
  owner 与 UID、activity、严格 LIFO guard、提前返回、异常恢复、session 生命周期和模拟线程边界；274/274
  assertions 通过。
- movement 和 `map::shift()` 会更新所有 human runtime 的绝对/泡内位置、route 和 remote-control 坐标；
  vehicle cache rebuild 后按每玩家稳定上下文重新解析 remote vehicle。mount、vehicle passenger/driver、grab、
  missions、map memory、diary、recipes、bionics 和 mutations 均完成双玩家 round-trip。
- `player_runtime` 使用 RFC 4122 v4 `player_id`、持久化 session generation 和
  `importing/active/offline/dead` 状态。schema version 1 的严格 player snapshot 可恢复两个独立 runtime 的
  avatar、装备、messages、stats、safe mode 和稳定 ID；反序列化保持 `importing`，registry 完成唯一性验证后
  才开始新 session。该 snapshot 是玩家边界证据，不冒充 ADR-0007 的 canonical generation world save。
- 旧单人存档字段和 JSON 结构保持不变；stats 261、messages 27、save 26，以及 map/teleport/vehicle/load
  139,624 个回归 assertions 均通过。
- 联合 ASan/UBSan/LSan（含 leak 和 stack-use-after-return）正向 274/274 通过，无 sanitizer finding；
  full-avatar move-swap 继续保留恰好 3 个 `!mayfail` identity assertion 失败作为负面对照。
- 本地 Android NDK `28.1.13356709` arm64 debug APK 编译通过。hosted baseline run `29205262759` 在同一
  提交上完成 Linux Clang、Android NDK 和 Windows MSVC 17.14 构建；Windows artifact 实际 `--version`
  输出逐行精确包含 `+tiles, +sound`。

## Phase 0 go/no-go 门禁

- 两个地址稳定的 avatar 连续切换活动上下文 10,000 次，对象地址、角色 ID 和稳定状态 hash 不变。
- inventory、nested pockets、worn、wielded、activity actor 中的 `item_location` 和 safe reference 始终解析到正确 owner 与 item UID。
- guard 支持严格 LIFO 嵌套、提前返回和异常恢复；非模拟线程使用在 debug/sanitizer 下触发断言，在 release 下不进入活动上下文。
- mounted、vehicle passenger、grab、remote control、missions、map memory、diary、recipes、bionics 和 mutations 完成上下文切换 round-trip。
- `creature_at()`、`shared_from()` 和 `critter_by_id()` 对活动与非活动 human player 始终返回稳定且正确的身份。
- player snapshot 保存/加载后两个玩家身份和玩家级状态一致，且不复制或修改共享 world/map/vehicle 状态；
  完整 world restart 与 generation fallback 属于 ADR-0007 和 Phase 5 门禁。
- ASan、UBSan、LSan 无错误，且显式 identity assertion 无逻辑失败。
- 上述门禁已经完成。后续生产扩展仍必须保持 registry ownership、持久 handle 和单模拟线程规则；若
  Phase 1 至 Phase 3 的真实 command loop 暴露地址或引用不变量失败，应立即重新打开或取代本 ADR。
