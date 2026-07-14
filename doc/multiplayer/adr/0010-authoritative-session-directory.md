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
3. session directory 在 simulation thread 上验证容量、identity/runtime、expected old generation 和 barrier state，原子
   提交或拒绝；
4. 成功后调用 lobby 的 `complete_authentication()`/`complete_resume()` 类入口，lobby 才更新 transport/token mirror
   并构造 response；失败则在任何 accepted response 前返回 typed rejection。

Generation 合约：

- resume token record 在服务器侧解析 expected old generation；当前 `ResumeRequest` 不新增 generation 字段；
- directory 使用 exact `expected_old -> new` API，只接受 `new == old + 1`；runtime/lobby/scheduler/client 全部使用该
  值，client accepted response 也必须验证精确 `+1`；
- canonical generation 必须小于 `INT64_MAX`，与 player snapshot 可反序列化范围一致；达到上限时拒绝新 session，
  不能生成随后无法加载的存档。

Disconnect 合约：

1. transport connection 进入 disconnected 或 graceful-release-pending；
2. directory 以 connection/session/generation 三元组拒绝 stale old-connection event；
3. 当前 barrier participant 进入 disconnected grace；runtime 保持 registry-owned 且可激活；
4. grace 内 resume 由两阶段 admission 原子换代；timeout 只在该 participant 成为 current slot 时执行一次 forced wait；
5. forced wait 成功并记录 terminal barrier state 后，directory 才允许 runtime offline 或 graceful release completion；
6. 重复 completion、timeout-after-resume、resume-after-terminal 和 stale disconnect 均必须幂等拒绝。

当前单玩家 active root 不能 offline。实现 disconnect 前必须在本 ADR 中选定并验证以下之一：

- 引入不属于玩家 session 的 neutral server root context；或
- 将 selected simulation context 与 runtime online/offline lifecycle 解耦，并允许安全重新绑定 root。

不得用临时 dummy player、跳过 offline transition 或提前销毁 runtime 规避该门禁。

若 scheduler execution fault 可能发生在非幂等 gameplay side effect 后，directory/server owner 必须停止模拟、记录
诊断并拒绝写新的 canonical save。只有明确发生在副作用前的失败才能正常保存。

## 替代方案

- lobby 继续先 accept，再由 simulation 追认：拒绝。simulation 失败时已经向客户端承诺了不存在的 session。
- lobby、runtime、scheduler 各自维护 generation：拒绝。resume 后必然存在分叉窗口，stale event 无法可靠判定。
- 在 resume request 增加客户端 generation：当前不采用。server token record 已能提供 expected old generation；
  无必要扩大 wire schema 和兼容矩阵。
- 断线时立即把 runtime offline：拒绝。会使 barrier-local forced wait 无法在正确 player context 中执行。
- fault 后保存当前内存：拒绝。没有 journal/rollback 时会固化不确定的部分 turn。

## 后果

- lobby 需要 pending connection stage 和 `complete_*` API，认证成功不再完全由网络线程决定。
- directory 成为 main/server orchestration 的 simulation-thread owner，并私有协调 runtime、scheduler 和 lobby completion。
- graceful release 也必须等 simulation/barrier commit 后再 ACK/删除 token，不能只以 socket drain 为完成条件。
- generation、disconnect 和 root-context 规则会增加状态机测试，但避免把 multiplayer conditionals散入 gameplay code。
- 在不修改 wire schema或平台条件的前提下，这一实现属于 Tier 1 Linux；若改变 protocol/public ABI，则升级到实际
  编译 production source 的定向 Tier 2。

## 验证要求

- 单元状态表覆盖 new auth、grace 内 resume、timeout-before-resume、resume-before-timeout、terminal 后 resume、
  graceful release、stale old-connection disconnect、重复 `complete_*` 和 queue/capacity failure。
- generation 覆盖 exact `+1`、错误 expected old、跳代、`INT64_MAX - 1` 边界、runtime/save round trip 和 client
  非精确增长拒绝。
- directory/lobby 测试证明 accepted response 只在 simulation commit 后生成；simulation rejection 不泄露 identity、
  不消耗 generation、不建立 active token record。
- disconnect 测试证明 forced wait 发生在 runtime offline 前且恰好一次；root-context 方案覆盖所有玩家断线后的
  world/save/fatal-shutdown 行为。
- Linux focused tests、完整 `[multiplayer]` 和生命周期 sanitizer 通过；接入 production 后增加 Linux headless
  server/native client PTY。保持 wire schema不变时不要求 Windows/Android；若修改 schema/public ABI，按 Tier 2
  补实际编译 changed production source 的 MSVC/NDK evidence。

## 接受门禁

在以下条件完成前保持“待验证”且 `players.max = 1`：

- root-context/lifecycle 方案在本 ADR 中选定；
- 两阶段 lobby admission、directory exact generation API 和 disconnect 状态表测试绿色；
- 单玩家 production scheduler/adapter/actual world claim 与 Linux PTY regression 绿色；
- execution-fault no-save 路径有 process-level evidence。
