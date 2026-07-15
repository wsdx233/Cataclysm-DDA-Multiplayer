# ADR-0011：Owned Turn 的双层事件泵边界

- 状态：已接受
- 日期：2026-07-15
- 接受日期：2026-07-15
- 关联计划：第 7.4、8、9、20.6 节与 Phase 3 Gate 2

## 背景

Gate 2 最初要求把 transport/event/admission pump 完全移出 remote action callback，目的是保证 initial unbound 与
root-dormant server 可以在不进入 `turn_begin`/`player_begin` 的情况下完成 auth/resume。production 接通时确认了
这个外层泵是必需的，但“任何时候都只能在 turn 外泵事件”与当前同步 `game::do_turn_impl()` 不兼容：

- 一个 accepted command 可能保留 moves，scheduler 返回 `accepted_remains_eligible`，同一个 player-input phase 会
  立即等待下一条 semantic command；native client 又串行等待上一条 command result；
- transport loss 后的 grace resume 必须在当前 barrier 仍 open 时由 simulation thread 提交 directory/runtime/
  scheduler/lifecycle exact transition，否则 timeout 与 resume 无法由同一个 owner 决胜；
- `do_turn_impl()` 当前没有可保存 C++ 栈与局部状态的 continuation，不能在 player-input 中途返回 outer loop，泵完
  网络后再从原位置恢复；
- 把 directory/lifecycle work 移到 network thread 会违反单模拟线程与 live game object 所有权约束。

严格 outer-only 的直接实现会产生可复现死锁：server 在 open player-input barrier 等下一条 command，而 client 等
command result 或 resume 后的 initial scene；双方都无法推进。

## 决策

Gate 2 使用一个 simulation-thread-owned 的**双层事件泵**，并固定允许边界：

1. **Outer boundary pump 是强制主入口。** unbound、root-dormant、between-turn 与任何尚未向 client 发布
   authenticated/resumed event + initial completed scene 的状态，都只能停留在 outer pump。此时不得调用
   `begin_turn()`、不得进入 `turn_begin`/`player_begin`、不得创建 active-player guard。
2. **Open player-input barrier 允许 bounded active-turn pump。** 只有 owner 已经打开 exact shared turn、执行栈正停在
   owned player-action wait、world 尚未 claim 时，simulation thread 才可在该 wait 内复用同一个 bounded pump，处理
   transport close、lobby control、exact confirmation、grace resume admission 与 semantic-command queue。该泵不是
   network-thread game execution，也不得越过 scheduler/current-slot/lifecycle 校验直接运行规则。
3. **world/player-end 禁止泵 control 或发布 live scene。** actual world thunk、player-end、lifecycle completion 与下一
   turn 保持同步且不可插入 admission/command execution。只有 owner 记录 exact player-end completion 后，才可构造并
   发布新的 live scene、interval save 或开始下一 turn。
4. **Open-turn resume/resync 只重放 immutable completed scene。** server 在 clean boundary 缓存上一次完成 turn 的
   scene payload。grace resume 或 resync 若发生在 open player-input barrier，只能把该 completed payload 换上当前
   exact session envelope 后重发；不得读取或发布 partial live game objects。
5. **Scene revision 表示 completed publish boundary。** player action 的 command result 保持最近 completed scene
   revision；同一 player phase 的 `accepted_remains_eligible` 因此不会要求尚不存在的 mid-turn scene。world +
   player-end + lifecycle completion 后 revision 才推进一次并发布新 scene。
6. active-turn pump 必须有明确的 bounded command queue，并优先处理 disconnect/admission/control，避免排队 command
   阻塞 terminal close。SIGINT 在 `begin_turn()` 前重新检查；open turn 中的 stop 仍按 fatal/no-save 处理。

未来若引入可暂停/恢复的 owned phase driver，可用后续 ADR 把 active-turn pump 完全移回 outer loop；Gate 2 不先把
这项大规模 continuation 重写作为 single-root lifecycle 的前置条件。

## 替代方案

- **严格 outer-only，当前 turn 栈不可恢复：拒绝。** `accepted_remains_eligible` 和 grace resume 会死锁。
- **每条 command 强制结束 player phase：拒绝。** 会丢弃合法剩余 moves，并破坏后续 round-robin 公平语义。
- **在 player action 后立即发布 partial live scene：拒绝。** lifecycle 尚未完成，违反 canonical publish/save
  boundary，也可能把 world 尚未执行的中间状态暴露给 client。
- **network thread 直接提交 directory/runtime 或运行规则：拒绝。** 违反 simulation-thread authority。
- **本 Gate 同时重写 `do_turn_impl()` 为 continuation/coroutine：暂缓。** 长期可取，但范围与回归风险显著超过
  single-root production lifecycle slice。

## 后果

- server loop 同时存在 outer pump 与受限 active-turn pump，但二者调用同一 simulation-thread event handler；状态
  归属不分叉。
- initial auth/dormant resume 必须等待 authenticated/resumed event 与 completed scene 实际 queued 后才能开始 turn。
- command result revision 不再等同于“每个 action 都产生新 scene”；它指向 client 可取得的最近 completed scene。
- 需要缓存 visibility-filtered、immutable completed-scene payload；缓存不授予读取 partial live state 的权限。
- Gate 2 文档、测试和日志必须明确区分 outer boundary pump 与 active player-input pump，不能笼统宣称事件泵已完全
  移出同步 owned turn。

## 实施发现与门禁校准

Gate 2 当前 production executor 只有 wait 和八方向 move，两者都会消耗本轮完整 standard move budget，因而没有一个
自然返回 `accepted_remains_eligible` 的真实 wire action。为制造 process case 而新增仅测试 gameplay command 会扩大
协议/规则面并削弱“production source evidence”的含义，因此接受门禁采用两层互补证据：

- owner/scheduler exact test 先返回 `accepted_remains_eligible`，证明同一 slot 保持
  `awaiting_command`/commandable，再执行第二条 accepted command，并完成 actual world/player-end/lifecycle boundary；
- 真实 headless process 在 open player-input barrier 内先返回 duplicate replay result，再接受下一条 fresh command。
  duplicate 与 remains-eligible 都要求 synchronous owned-turn stack 在发出 result 后继续 active-turn pump，而不能依赖
  mid-turn scene，因此该进程覆盖本 ADR 要避免的“result 后等待下一 command”死锁形状。

未来任何 production executor 若能自然保留 moves，在启用该 executor 前必须增加真实 process
`accepted_remains_eligible -> second command` 回归；当前接受不自动覆盖该未来动作。

initial/dormant 门禁同样以 owner state assertion 与真实进程互补：owner tests 证明 fresh auth 前和 dormant 中
`can_begin_turn() == false`，same-runtime resume 后才恢复；process test 证明 initial auth 先发布 completed scene，最终
disconnect 超过 grace 后可在 safe dormant boundary 正常保存。无需把每个 `do_turn()` observation trace 写入 production
日志；若后续改动绕过 owner begin-turn gate，应新增定向 trace regression。

## 验证要求

- owner state/phase test 证明 initial unbound 与 dormant resume 在 authenticated/resumed + initial scene 前不能开始
  owned turn；若改动绕过 owner gate，再补 production phase trace。
- owner exact test 覆盖 `accepted_remains_eligible` 后的第二条 command；当前真实 process 以 duplicate-result 后的
  fresh command 覆盖同一 active-turn pump deadlock 形状。未来自然 remains-eligible executor 启用前必须补 process case。
- grace resume 在 open barrier 内完成 exact generation transition，client 收到缓存的 completed scene 后能 replay/
  继续 command，actual new scene 只在 player-end/lifecycle completion 后发布。
- resync 在 open barrier 内只重放 cached completed payload；测试禁止 scene builder 读取 partial live state。
- disconnect/control 优先于 bounded queued commands；SIGINT safe-boundary 与 open-turn no-save 路径均有证据。
- Linux focused/full/sanitizer 与 headless-server/native-client PTY 通过。该决策不修改 wire schema 或平台 owned code，
  routine Gate 2 验证保持 Linux-first；只有后续 public/platform boundary 变化才升级对应 Tier 2 gate。

## 实施证据

source `2e9236c7bf91782ad3f15daa5d8aa0e3929f355a` 的 production server 使用 outer pump 处理 initial/dormant/
between-turn admission，并只在 owned player-action wait 中进入 bounded active-turn pump。completed-scene payload 在
clean boundary 缓存；open-barrier raw close/resume 后的 scene 与此前 resync payload 逐字节一致，open barrier 中再次
resync 仍逐字节一致。command result statuses 为 `0, 2, 0`，scene revisions 为 `1 -> 2 -> 3`；三次 resume 后
changed-payload sequence conflict 触发 terminal close，且 close-priority 测试证明同批合法 command + malformed control
不会执行 gameplay。

真实 curses PTY 完成 wait、forced transport drop、generation-2 resume、uncertain-command duplicate、east move 和
clean quit，client/server 均退出 `0`。另一个真实进程在 authenticated open turn、尚无 command 时 SIGTERM，server
退出 `1`，记录 `save_refused(disposition=2)`/`runtime_failed`，没有 `save_completed`，save SHA-256 不变。完整
`[multiplayer]` 为 125 cases / 7,174 assertions（仅 2 个既有 mayfail identity 对照），final-source owner/server/lobby
ASan/UBSan/LSan 为 27 cases / 3,923 assertions，无 finding。Windows/Android 按 Tier 1 未运行。

## 接受门禁

上述 owner-state、multi-command、open-barrier cached-scene、close-priority、fault/no-save 与 Linux PTY 证据均已记录，
因此本 ADR 于 2026-07-15 接受。若未来自然 remains-eligible action 的 process 回归失败，或 active-turn pump 无法继续
限制在 player-input/world-before-claim 边界，应停止扩展该路径并设计 resumable owned phase driver。
