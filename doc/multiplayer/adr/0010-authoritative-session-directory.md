# ADR-0010：权威 Session Directory 与两阶段 Admission

- 状态：待验证
- 日期：2026-07-14
- 关联计划：第 6.2、7.4、8.5、20.6 节与 Phase 3

## 背景

Phase 2 的单远程玩家路径把 transport lobby、resume-token record、server-owned runtime 和主循环连接起来，但这些
对象没有一个共同的 session transition owner：

- lobby 当前在 `handle_authenticate()`/`handle_resume()` 内先生成 identity/session、递增 generation、更新 token
  record 并构造 accepted response，之后才把 event 交给 simulation thread；
- `multiplayer_player_runtime::begin_session()` 只允许 importing/offline runtime 并隐式自增 generation，无法在
  disconnected grace 期间仍为 active 的 runtime 上原子应用 resume；
- scheduler 只持有当前 turn 的 participant snapshot，不应拥有长期 connection/token lifecycle；
- `game::disconnect_multiplayer_player()` 拒绝把当前 active root runtime 置为 offline，单玩家 server 没有另一个
  active runtime 可切换；
- lobby/scheduler 使用 `uint64_t` 上限，runtime/save 使用 `INT64_MAX` 边界，client 又只验证 generation 变大而不是
  精确 `+1`。
- 原计划只让服务器 resume-token record 提供 expected old generation，但这无法区分两种断线：accepted resume
  response 在客户端收到前丢失，或客户端已收到 response 后再次断线。两者可携带相同 token/revision/sequence，若
  request 不声明客户端最后接受的 generation，服务器无法安全选择重放已提交的 `g + 1` 还是推进到 `g + 2`。

若直接把现有对象互相回调，会出现成功响应早于 runtime/admission commit、generation 分叉、stale disconnect 覆盖
新 session，或 forced wait 前 runtime 已不可激活等问题。

## 决策

引入只在 simulation thread 运行的 authoritative session directory，并固定所有权：

| 组件 | 拥有 | 不拥有 |
| --- | --- | --- |
| Network lobby | transport connection、wire/UTF-8/rate-limit validation、resume token、replay window、pending request | canonical identity/runtime generation、barrier admission、avatar lifecycle |
| Session directory | stable player/character identity、runtime binding、canonical generation、connection admission、disconnect/grace/offline transition | socket I/O、frame codec、game-rule execution |
| Player registry/runtime | 地址稳定 avatar/runtime storage 与 directory 状态的可查询镜像 | 独立 session admission 或 generation 决策 |
| Turn scheduler | 当前 shared turn 的 immutable participant snapshot 和 barrier state | 长期 token、connection 或 runtime ownership |

Auth/resume 使用两阶段 admission：

1. lobby 验证 wire request、token/rate limit/replay floor，保留 pending connection state，但不发送 accepted response、
   不递增 canonical generation；
2. lobby 向 simulation queue 发出 pending authentication/resume request；
3. session directory 在 simulation thread 上验证容量、identity/runtime、expected old generation 和 barrier state，只
   生成带 directory version 的 admission plan；lobby 用该 plan 预编码 accepted/rejected response，避免 canonical
   commit 后才发现 response 无法构造；
4. accepted plan 先由 directory 原子提交 exact generation 与 connection tuple；rejected plan 不改 directory；
5. server 将预编码 response 作为一个 transport command 入队，rejection 使用 ordered `send_and_disconnect`。只有入队
   成功后 lobby 才发布 token mirror 与 post-commit event；accepted response 若无法入队，directory 清除 connection
   binding，但已经提交的 generation 不回滚。resume 保留原 token mirror，允许下一次 exact-fingerprint request 由
   directory 重放已提交 generation 并修复 mirror；fresh auth 尚未发布 token record，后续 fresh retry 是新的
   admission，不承诺重放同一代。

Generation 合约：

- `ResumeRequest` 携带客户端最后接受的 `session_generation`；protocol minor 随该不兼容边界递增。服务器 token
  record 是 lobby-owned wire/replay mirror，directory/runtime entry 是 authoritative simulation state。lobby 先把
  request 与 token mirror 核对并产生 immutable pending DTO，directory 再把 DTO 与 entry/runtime/fingerprint 核对；
  directory 不跨线程直接读取 token map，两层都不能信任客户端单独声明的值；
- 普通 resume 使用 exact `expected_old -> new` API，只接受 `new == old + 1`；runtime/lobby/scheduler/client 全部使用
  该值，client accepted response 也必须验证精确 `+1`；
- 若上一条 accepted resume 已由 directory 提交但 response 未被客户端确认，后续 request 仍携带上一代
  `expected_old`。通常 directory/runtime 与 token mirror 当前都为 `expected_old + 1`；若 accepted response 在 enqueue
  前失败，token mirror 可以暂时仍为 `expected_old`，但 directory/runtime 必须已为 `expected_old + 1`。只有 connection
  已断开且 revision/client sequence 与 directory 上一条 committed resume fingerprint 精确匹配时，才可把两种情况都
  作为幂等 completion replay：为新 transport session 重发同一 canonical generation，不再次递增，并在 enqueue
  成功后把 lobby mirror 修复到该代。其他回退、跳代或旧 fingerprint 一律 `session_expired`；
- 客户端在新 session 上发出的第一个有效 application frame 证明它已经收到 accepted response，并触发带 exact
  connection/session/player/character/generation tuple 的 confirmation。只有 simulation-thread directory 接受该
  confirmation 后，authoritative fingerprint 才视为消费，server/main 再显式 ack lobby 完成 mirror confirmation；
  lobby 在 ack 前只标记 confirmation pending，不能提前不可逆删除 fingerprint。排队、tuple 或 ack 失败必须
  fail-stop，不能留下两侧分叉。ping 由 server wrapper 转成内部 confirmation event，普通
  command/resync/disconnect 随原 event 在 simulation thread 确认；
- fresh authentication 的新 token 在第一条有效 application frame 前视为客户端尚未确认。该连接若先断开，lobby
  删除客户端可能根本不知道的 inactive record，允许 fresh authentication 重试。会导致客户端清 token 的终态
  `session_expired` 也必须撤销 inactive record；active/pending 冲突返回可重试的 `invalid_state`，不能制造 max-player
  容量锁死；
- canonical generation 必须小于 `INT64_MAX`，与 player snapshot 可反序列化范围一致；达到上限时拒绝新 session，
  不能生成随后无法加载的存档。
- server 启动时可一次采用 registry 已 active 的稳定 root runtime/current generation 作为 bootstrap；entry 建立后所有
  后续 transition 都必须经 directory version 与 exact API，不能再次用 adopt 绕过 generation owner。

Disconnect 合约：

1. transport connection 进入 disconnected 或 graceful-release-pending；
2. directory 以 connection/session/generation 三元组拒绝 stale old-connection event；
3. 当前 barrier participant 进入 disconnected grace；runtime 保持 registry-owned 且可激活；
4. grace 内 resume 由两阶段 admission 原子换代；timeout 只在该 participant 成为 current slot 时执行一次 forced wait；
5. forced wait 成功并记录 terminal barrier state 后，directory 才允许 runtime offline 或 graceful release completion；
6. 重复 completion、timeout-after-resume、resume-after-terminal 和 stale disconnect 均必须幂等拒绝。

当前单玩家 active root 不能 offline。本 ADR 选择 **selected simulation context 与 session/runtime lifecycle 解耦**
作为 `players.max = 1` 的最小可逆方案：

- selected root 只是 legacy getter 的地址稳定兼容上下文，不代表该 runtime 在线或允许执行命令；
- 最后一个连接断开或 release 时，runtime 在当前 barrier forced wait 和 actual world phase 完成前继续保持 active；到达
  canonical turn boundary 后才进入 offline，并把 server 标记为 root-dormant；
- dormant 状态不启动新 turn、不创建 active-player guard、不执行玩家命令，只允许安全点上的 save/shutdown；
- resume 在同一地址稳定 runtime 上先完成 exact generation admission 并恢复 active，再解除 dormant；
- guard 与普通 `set_active_player()` 继续只接受 active runtime，不全局放宽为 offline。未来启用第二玩家时，在安全点
  确定性选择另一 active runtime；没有 active runtime 才进入 dormant，届时再评估是否需要 neutral root。

不得用临时 dummy player、跳过 offline transition、提前销毁 runtime，或在 offline root 上继续运行 world phase
规避该门禁。

若 scheduler execution fault 可能发生在非幂等 gameplay side effect 后，directory/server owner 必须停止模拟、记录
诊断并拒绝写新的 canonical save。只有明确发生在副作用前的失败才能正常保存。

### Phase 3 Gate 1 实施证据（2026-07-15）

source `cbd19b48d652be735a3c83fe841d2d7c831aecba` 已完成 selected-root lifecycle contract/API，但没有接入
production owner：

- `multiplayer_selected_root_lifecycle` 作为 pure cross-component contract 独立记录 connection、barrier、runtime、
  departure 和 server dormant/fault 状态；它不拥有 socket、runtime、scheduler、callback 或 game object，只在调用方
  声明 exact external effect 已完成后记录 transition。
- owner/version/epoch-bound departure ticket 拒绝 stale/cross-owner completion；owner/version-bound admission recipe
  分开 published 与 unpublished scheduler effect。recipe 不证明 directory commit，production owner 必须私有封装
  directory plan/commit、recipe、enqueue/publish/cleanup 和 lifecycle record，或未来由 directory 返回 opaque receipt。
- directory commit 记录 unpublishable admission 应保持 active 还是恢复 offline，调用方不再选择 cleanup mode；
  graceful-release-pending 保留 exact tuple 但取消 commandability；exact offline 要求 simulation thread、无 active guard、
  无 commandable binding 和统一 player/character/generation/runtime mirror。
- runtime 支持 exact-generation offline 与 same-generation reactivation；scheduler 支持 disconnected same-generation
  replay rebind 和保持 disconnect grace 的 committed `+1` generation repair。
- 状态表覆盖 boundary、awaiting、terminal 和 world-processing departure，timeout/resume 单赢家，forced wait/world/
  offline 顺序，caller-reported graceful ACK-queued/early-close，same stable runtime resume，stale/duplicate work，以及
  dormant guard/save gate。
  real directory/runtime/scheduler 组合覆盖普通 disconnect 到 dormant/resume、terminal graceful release 和 unpublished
  grace `+1` repair。

这批 API 是调用方见证型顺序契约，不是跨组件事务证明。Gate 2 必须把“外部 effect 已成功，但对应 `record_*`
不是 applied/duplicate”统一升级为 `latch_fault()` 和 typed fatal shutdown；不得重试可能已发生的非幂等 side effect。
只有非幂等 gameplay/world side effect 可能已发生，或 canonical state 已无法证明时才进入 no-save；明确发生在这些
副作用前的失败仍可走正常 save。
此外，当前 `multiplayer_dedicated_server::complete_graceful_disconnect()` 只观察 lobby 是否产生
`send_and_disconnect` action，而 transport enqueue/fallback 结果被忽略。Gate 2 在记录 ACK queued 前必须显式区分
queued、queue rejection/fallback close 和 peer early close。

Production shutdown/save 同样受 lifecycle safe boundary 约束。当前 remote action wait 若因 SIGINT 返回空结果，可能
已经执行 turn-begin/player-begin 但尚未执行 world；Gate 2 不能把这种 open partial turn 当作普通保存点。只有 clean
connected boundary 或 dormant safe point 可 normal-save；open turn 或 canonical state 不可证明时进入 fatal/no-save。

## 替代方案

- lobby 继续先 accept，再由 simulation 追认：拒绝。simulation 失败时已经向客户端承诺了不存在的 session。
- lobby、runtime、scheduler 各自维护 generation：拒绝。resume 后必然存在分叉窗口，stale event 无法可靠判定。
- 只从 server token record 推断客户端 generation：拒绝。accepted response 丢失时存在无法区分的 `g + 1`/`g + 2`
  歧义，不能闭合 exact-generation 合约。
- 每次 resume 轮换 token 并在 result 返回新 token：可解决同类歧义，但会扩大 token 双代保留、write-ack 与恢复状态；
  当前选择 request 显式携带 last accepted generation，并对上一 committed result 做单代幂等 replay。
- 断线时立即把 runtime offline：拒绝。会使 barrier-local forced wait 无法在正确 player context 中执行。
- fault 后保存当前内存：拒绝。没有 journal/rollback 时会固化不确定的部分 turn。

## 后果

- lobby 需要 pending connection stage、commit 前 `prepare_admission()` 和 enqueue 后 `publish_admission()`；认证成功
  不再完全由网络线程决定。
- directory 成为 main/server orchestration 的 simulation-thread owner，并私有协调 runtime、scheduler 和 lobby completion。
- graceful release 也必须等 simulation/barrier commit 后再 ACK/删除 token，不能只以 socket drain 为完成条件。
- generation、disconnect 和 root-context 规则会增加状态机测试，但避免把 multiplayer conditionals散入 gameplay code。
- 本次 `ResumeRequest.session_generation`/protocol minor `1` 是公共 wire boundary：Linux production gate 之外，需
  增加实际编译 changed production source 的定向 MSVC/NDK evidence。后续不改变 schema/public ABI 的内部 session
  policy 迭代恢复为 Tier 1 Linux-first，不因此每次重跑完整 package matrix。本次不宣称 phase exit、release 或旧新
  minor 互通，故是 Tier 2 而非 Tier 3 protocol-compatibility milestone；当前 CI 暂用 Windows/Android actual-source
  package job 作为编译 fallback，不把 package/runtime 行为本身加入本 ADR 的验收范围。

## 验证要求

- 单元状态表覆盖 new auth、grace 内 resume、timeout-before-resume、resume-before-timeout、terminal 后 resume、
  graceful release、stale old-connection disconnect、重复 prepare/publish 和 queue/capacity failure。
- generation 覆盖 exact `+1`、错误 expected old、跳代、`INT64_MAX - 1` 边界、runtime/save round trip 和 client
  非精确增长拒绝；accepted response 丢失后相同 generation/revision/sequence 的 retry 必须重放同一 committed
  generation，而客户端已接受后使用新 generation 的下一次 resume 必须再推进一代。
- directory/lobby 测试证明 response 可以在 simulation commit 前预编码，但 accepted bytes 与 token/event publish 只能在
  commit 后发生；simulation rejection 不泄露 accepted identity、不消耗 generation、不建立 active token record。
- 状态表覆盖 fresh auth response 未确认即断线、首个 application frame 触发 exact-tuple directory confirmation、
  confirmation 后消费 replay fingerprint 与显式 lobby ack、ack 前不清 mirror、终态 `session_expired` 撤销
  inactive token、active/pending `invalid_state` 保留 token，以及 rejection 的单 command ordered close/queue-full
  fallback；客户端清 token 后不得被遗留 record 锁在 `server_full`。
- disconnect 测试证明 forced wait 发生在 runtime offline 前且恰好一次；root-context 方案覆盖所有玩家断线后的
  world/save/fatal-shutdown 行为。
- Linux focused tests、完整 `[multiplayer]` 和生命周期 sanitizer 通过；接入 production 后增加 Linux headless
  server/native client PTY。保持 wire schema不变时不要求 Windows/Android；若修改 schema/public ABI，按 Tier 2
  补实际编译 changed production source 的 MSVC/NDK evidence。

Gate 1 source 的 Linux evidence：root lifecycle + directory + scheduler 为 36 cases / 1,257 assertions；完整
`[multiplayer]` 为 104 cases / 6,150 assertions，其中 2 个既有 `[!mayfail]` full-avatar move-swap case 保留 3 个
expected failures；定向 ASan/UBSan/LSan 为 46 cases / 1,489 assertions，无 finding。该 diff 未修改 wire/schema/
external public ABI、platform conditional 或 Windows/Android-owned source；新增 lifecycle header 是 internal shared
C++ API。按第 20.6 节 Tier 1 未重复跨平台 package/runtime。

## 接受门禁

在以下条件完成前保持“待验证”且 `players.max = 1`：

- Gate 1 selected-context/lifecycle decoupling contract/API 已完成；production owner 仍须实际执行 dormant/root 恢复；
- 两阶段 lobby admission、directory exact generation API 和 disconnect 状态表测试绿色；
- 单玩家 production scheduler/adapter/actual world claim 与 Linux PTY regression 绿色；
- execution-fault no-save 路径有 process-level evidence。
