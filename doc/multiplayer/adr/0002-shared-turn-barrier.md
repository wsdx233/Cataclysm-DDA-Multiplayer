# ADR-0002：共享回合屏障与公平调度

- 状态：已接受
- 日期：2026-07-12
- Phase 3 实施注记：2026-07-15
- 关联计划：第 8、9、20 节
- 关联 session ownership：[ADR-0010](0010-authoritative-session-directory.md)（待验证）

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

Phase 3 的第二个 source slice 增加不可复制/移动的 `multiplayer_turn_phase_adapter`，把纯 policy 与 live rule
execution 之间的最低顺序固定为：

- 在模拟线程验证 exact current slot、`player_id`、session generation、runtime registry ownership/lifecycle 和构造时
  记录的 root active context。
- 在目标 `multiplayer_active_player_guard` 内先执行共享 action/wait rule；accepted action 的
  `action_taken()` bookkeeping 也在同一 guard 内完成。guard 恢复成功后才记录 scheduler disposition。
- authoritative forced wait 绕过玩家请求的 safe-mode permission gate，但仍执行真实 `Character::pause()`；
  `check_safe_mode_allowed()` 的直接玩家读取使用 active avatar，不再固定读取 host backing avatar。
- callback 抛出/拒绝完成、context 恢复失败、wait/world callback 失败，或已经执行规则后 scheduler record 失败，均
  latch scheduler execution fault。fault 后所有 transition、新 adapter 和新 turn 都被拒绝，避免重试非幂等副作用；
  world failure 保留 `world_processing` 供诊断。

这些 slices 仍不拥有 production session、socket 或 command payload。特别地：

- Gate 1 source `cbd19b48d652be735a3c83fe841d2d7c831aecba` 增加 pure
  `multiplayer_selected_root_lifecycle`，把 scheduler participant、directory binding/runtime key、departure ticket、
  shared turn/world ticket 和 dormant/fault gate 组成 owner/version-bound effect contract。scheduler 同时增加 exact
  same-generation replay rebind 与 disconnected committed `+1` repair；两者都只改变当前 immutable barrier snapshot
  的 generation metadata，不取得长期 session ownership。
- adapter 已在 in-process tests 中证明真实 forced wait 的 execute-before-record 和 guard 恢复，但目前只接受
  `active` runtime。断线 orchestration 必须把 transport disconnected、barrier disconnected 和 runtime offline 分开，
  在 forced wait 完成前保持 registry owner 可激活。
- production session directory 已成为 canonical generation owner，runtime 由 directory 精确推进，lobby 只保留受控
  token/replay mirror，并实现 `pending -> plan -> pre-encode -> commit -> transport enqueue -> mirror/event publish`。
  `ResumeRequest` 携带客户端最后接受的 generation，由 token/runtime
  分层交叉验证；上一 accepted response 丢失只按相同 revision/sequence fingerprint 重放同一代。首个有效
  application frame 触发 exact-tuple directory confirmation，只有该 simulation-thread confirmation 接受后才消费
  authoritative replay permission，并由 server/main 显式 ack lobby mirror。scheduler participant 仍只在
  turn-local snapshot 中读取 directory 提供的代数。
- runtime 已有 directory-only exact `expected_old -> new` transition；bootstrap 只允许一次采用 registry 已 active root
  的有效 generation，后续不能绕过 directory version。
- 单玩家 server 的 active root runtime 仍不能通过 `game::disconnect_multiplayer_player()` 直接进入 offline。ADR-0010
  已选择 selected-context/lifecycle decoupling，但 production 还未实现 barrier/world 后的 offline/dormant 和 resume
  reactivation；所有玩家断线后的 world/save/shutdown 测试仍是门禁。
- world ticket 与 adapter 目前只包裹测试 lambda；真实 `process_legacy_single_player_bubble_turn()` 仍由
  `do_turn_impl()` 直接调用。因此尚未证明 actual bubble callback 在 claim 后恰好执行一次。
- execution fault 当前是进程内 fail-stop，不是 retry、rollback、save/restart 或 typed shutdown recovery protocol；
  production owner 必须把 fault 提升为不可继续模拟的 session/server 状态。
- 若 execution fault 可能发生在 `pause()`、player action 或 world callback 等非幂等副作用之后，server 必须停止模拟、
  记录诊断并拒绝写新的 canonical save；没有 journal/rollback 时保存会固化不确定的部分 turn。只有明确分类为
  pre-side-effect 的 failure 才可走正常保存路径。
- 当前 production server 已用 authoritative directory binding 替代单一 `active_remote_session`，但仍只路由固定 root
  player，command cache 仍只按 sequence 索引，也没有使用 shared scheduler；配置继续拒绝 `players.max > 1`。
  本注记不代表两玩家 server 或 Phase 3 退出标准已经完成。

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
- session directory 只在 simulation thread 提交 auth/resume/disconnect；network lobby 不能先发送成功响应或独立
  修改 canonical generation。pre-grace resume、grace 内 resume、timeout-before-resume、resume-before-timeout、stale
  disconnect event 和重复 completion 都必须有明确状态表与幂等测试。

## 验证要求

- 单元测试覆盖 round-robin、首位轮换、moves 耗尽、wait、timeout、断线和重连。
- 单元测试覆盖 immutable roster snapshot、roster churn、四玩家无饥饿、rejected/duplicate 不推进、精确
  generation resume、仅 current slot timeout、`automatic_wait_pending` 和 world ticket 的 stale/double-call 拒绝。
- 集成测试必须通过 existing registry/guard 执行真实 wait，并增加故意绕过或失败的 adapter 测试；直接调用
  `record_automatic_wait_executed()` 不能作为 gameplay 执行证据。
- phase-adapter 单元测试覆盖 exact slot/generation/ownership/root context、active-avatar safe-mode permission、真实
  forced pause、stale/missing/inactive runtime、callback/wait/world failure、post-side-effect scheduler-record fault 和
  replacement-adapter 拒绝；当前
  two-runtime wait-only test 的 world callback 是计数 lambda，不是 actual bubble gameplay evidence。
- production 集成测试必须把真实 legacy bubble callback 放在 claim 后计数，并覆盖 failure 后的 typed fail-stop/
  shutdown；单测只观察到一次 `claim_world()` 或 lambda 不足以证明 world phase exactly-once。
- production owner 测试必须证明 external scheduler/runtime/directory/world effect 成功而 lifecycle record 失败时立即
  latch fault，并区分 graceful ACK 实际 queued、fallback close 和 peer early close；pure lifecycle 的
  applied/duplicate 状态本身不是外部 effect 或 transport enqueue 的证明。
- owned production turn seam 必须在 action callback 被 sleep/activity/zero-moves 跳过时仍发出不可绕过的
  pre-world/player-phase-complete hook，使 participant terminal state 与 world claim 对齐；不得让 awaiting scheduler
  直接进入 legacy world，也不得伪造 accepted command result。scheduler world completion 可在 world hook 内记录；
  lifecycle/owner 的 canonical turn-boundary completion 必须等后续 player-end 完成，scene/save/next turn 均在其后。
- 两玩家同时拾取同一物品时只成功一次，失败方收到 `state_changed` 和新 revision。
- 一名玩家制作或睡眠、另一名玩家普通行动时，世界时间和活动进度保持一致。
- 四客户端 soak test 中不存在饥饿调度、重复 world phase 或无限 barrier 推进。
