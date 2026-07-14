# ADR-0002：共享回合屏障与公平调度

- 状态：已接受
- 日期：2026-07-12
- Phase 3 实施注记：2026-07-14
- 关联计划：第 8、9、20 节

## 背景

当前主循环先让唯一 avatar 消耗 moves，再处理地图、怪物、NPC、天气和其他世界阶段。多人模式如果让每名玩家独立推进时间，会导致同一世界处于多个时间点；如果按真实时间持续推进，则菜单、阅读、制作和睡眠等回合制交互会变得不可控。

多人调度还必须处理两个玩家竞争同一物品、门、位置或目标时的顺序，并避免房主或先连接玩家永久获得优先权。

## 决策

服务器使用共享的一秒回合屏障：

- 每个世界 turn 开始时，为所有参与 barrier 的在线玩家执行 turn-begin 处理并准备 moves。
- 按稳定的 round-robin 顺序，每轮最多执行每名仍有 moves 的玩家一个命令。
- 每个 turn 的首位玩家轮换，冲突按服务器实际执行顺序裁决并记录 ordering key。
- 玩家仍可行动但尚未提交命令时，世界停在当前 barrier；客户端本地菜单不能阻塞服务器 C++ 调用栈。
- 长活动由服务器自动执行 activity step。只有所有在线玩家都处于允许快进且安全的状态时才批量推进。
- 所有参与者耗尽 moves、明确 wait 或被策略自动 wait 后，服务器只执行一次 bubble/world 阶段并发布新 revision。
- 默认断线宽限期内保留 barrier；超时后按服务器策略移出 barrier，角色留在世界中并自动 wait。

服务器策略可配置 `wait_forever`、`auto_wait_after_seconds`、`pause_when_disconnected` 和 `host_controls_pause`，但不能改变“一次 world phase 对应一个共享 turn”的不变量。

Phase 3 的首个实现切片把上述调度规则收口为纯策略状态机，采用以下具体合约：

- scheduler 实例不可复制或移动，避免复制 barrier/world claim 状态；`begin_turn()` 复制 1 至 4 名参与者的
  roster，该 snapshot 在本 turn 内不可变，中途加入者只能进入下一 turn。
- `player_id` 是稳定 roster 身份，session generation 是每次 barrier 转换都要核对的可变元数据。断线恢复必须提供精确旧 generation，并只允许递增 `1`。
- roster 先按稳定 `player_id` 排序；下一 turn 的首位是上个已完成 turn 首位的字典序后继，不存在时回绕。这样 roster 增删不会退化为固定 host-first。
- 每个 slot 只接受 typed disposition：`accepted_remains_eligible`、`accepted_finished`、`rejected` 或
  `duplicate`。后两者不消费调度机会、不改变 round/cursor；accepted 结果才按状态推进。
- 断线标记属于当前 barrier snapshot，不替代权威 session directory。timeout 只可作用于当前
  `disconnected_grace` slot；`automatic_wait` 和 `remove_from_barrier` 都先进入
  `automatic_wait_pending`，不能直接跨过玩家规则执行。
- 调度器只有在所有 snapshot 参与者 terminal 后进入 `world_ready`。编排器通过 `claim_world()` 把状态转换为
  `world_processing` 并取得执行当前 shared turn world phase 的 permission marker；
  `record_world_completed()` 只在 `world_processing` 且 shared turn 匹配时记录完成，之后才能开始下一 turn。

这个切片只决定顺序和状态，不拥有 avatar、socket、命令 payload 或 gameplay callback。特别地：

- `record_automatic_wait_executed()` 只表达编排器声称“权威 wait 已成功执行”；纯状态机无法证明真实
  `multiplayer_execute_wait()` 已在正确 `multiplayer_active_player_guard` 下运行。生产接入必须由不可绕过的
  forced/scoped adapter 先执行 wait，再提交状态。
- world ticket 只保证 scheduler 的 `world_ready -> world_processing -> idle` claim/record 顺序，不能证明真实
  bubble/world callback 恰好执行一次。当前也没有 world callback 失败后的 retry、abort、rollback 或恢复协议；这些
  必须在生产 phase adapter 门禁中补齐。
- 当前 production server 仍是单一 `active_remote_session`，没有使用该 scheduler，配置仍拒绝
  `players.max > 1`。本注记不代表两玩家 server 或 Phase 3 退出标准已经完成。

## 替代方案

- 世界按墙钟持续运行：拒绝。与长活动、复杂菜单和回合制风险判断不兼容。
- 每名玩家独立时间线：拒绝。无法为共享地图、怪物、车辆和物品定义一致状态。
- 客户端 lockstep：拒绝。把确定性和掉线恢复压力转移给所有客户端，且会泄露或复制权威状态。
- 固定 host-first 顺序：拒绝。会造成永久冲突优势。

## 后果

- 一个未提交动作的玩家可以暂停整个私服世界，需要清晰的等待、超时和管理员策略。
- 客户端菜单必须拆成 query/choice/commit，不能让服务器等待远程按键。
- 快进优化必须保留危险检查、活动中断和可恢复检查点。
- 调度器需要显式 revision、client sequence 和幂等结果缓存，以处理重连时的未知执行结果。
- session ownership、deadline clock、权威自动 wait、world callback 执行和失败恢复属于编排层，不应伪装成纯
  scheduler 已经提供的保证。

## 验证要求

- 单元测试覆盖 round-robin、首位轮换、moves 耗尽、wait、timeout、断线和重连。
- 单元测试覆盖 immutable roster snapshot、roster churn、四玩家无饥饿、rejected/duplicate 不推进、精确
  generation resume、仅 current slot timeout、`automatic_wait_pending` 和 world ticket 的 stale/double-call 拒绝。
- 集成测试必须通过 existing registry/guard 执行真实 wait，并增加故意绕过或失败的 adapter 测试；直接调用
  `record_automatic_wait_executed()` 不能作为 gameplay 执行证据。
- phase-adapter 测试必须对真实 world callback 计数，并覆盖 callback 失败后的明确策略；单测只观察到一次
  `claim_world()` 不足以证明 world phase exactly-once。
- 两玩家同时拾取同一物品时只成功一次，失败方收到 `state_changed` 和新 revision。
- 一名玩家制作或睡眠、另一名玩家普通行动时，世界时间和活动进度保持一致。
- 四客户端 soak test 中不存在饥饿调度、重复 world phase 或无限 barrier 推进。
