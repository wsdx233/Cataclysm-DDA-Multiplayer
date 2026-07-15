# CDDA 多人 fork 架构决策记录

本目录记录 [`MULTIPLAYER_REFACTOR_PLAN.md`](../MULTIPLAYER_REFACTOR_PLAN.md) 中关键架构选择的背景、取舍、后果和验证门禁。重构计划仍是产品范围与总体架构的来源；ADR 用于保留决策历史。两者发生变化时必须在同一改动中保持一致。

## 状态

- `已接受`：当前实现必须遵守，修改时需要新增替代 ADR 或更新现有 ADR，并同步重构计划。
- `待验证`：方向已确定，但该 ADR 写明的接受门禁尚未闭合，不能作为生产实现的既成事实。
- `已取代`：由后续 ADR 替代，保留用于说明历史原因。
- `已拒绝`：方案经过评估后不采用。

## 决策索引

| ADR | 决策 | 状态 |
| --- | --- | --- |
| [0001](0001-server-authoritative-simulation.md) | 服务器权威与单模拟线程 | 已接受 |
| [0002](0002-shared-turn-barrier.md) | 共享回合屏障与公平调度 | 已接受 |
| [0003](0003-asio-tcp-and-tls-policy.md) | standalone Asio/TCP 与 TLS 发布策略 | 已接受 |
| [0004](0004-semantic-flatbuffers-protocol.md) | 语义命令、可见状态与 FlatBuffers 协议 | 已接受 |
| [0005](0005-active-avatar-player-bridge.md) | 稳定 avatar 地址与活动玩家上下文 | 已接受 |
| [0006](0006-visibility-filtered-remote-scene.md) | 可见性过滤的远程场景与客户端本地渲染 | 已接受 |
| [0007](0007-server-canonical-generation-saves.md) | 服务器 canonical generation 存档 | 已接受 |
| [0008](0008-portable-character-policy.md) | Portable Character 与人物所有权策略 | 已接受 |
| [0009](0009-single-reality-bubble-v1.md) | v1 单 reality bubble 与距离约束 | 已接受 |
| [0010](0010-authoritative-session-directory.md) | 权威 session directory、两阶段 admission 与 generation ownership | 待验证 |

## ADR 维护规则

每个 ADR 至少包含状态、背景、决策、替代方案、后果和验证要求。处于 `待验证` 状态的 ADR 必须写明 go/no-go 条件
和失败退路。完成对应验证与接受门禁后，应在同一改动中更新状态、记录证据，并同步重构计划和已知限制。

ADR 不替代测试，但 ADR 的验证要求也不表示每个中间提交都要重跑所有平台。具体节奏与平台选择遵循重构计划
第 20.6 节：编辑循环使用 Linux incremental/focused tests，slice 收口时按风险增加 full/sanitizer/PTY，关键
platform-owned/public-boundary 批次完成后才运行一次受影响平台门禁。phase exit 仍以当前 Linux 候选完成主门禁；
未变化平台可以引用最近兼容 evidence commit 加显式 diff audit，但必须注明当前候选未在该平台编译或运行。release、
toolchain/artifact/signing 变化和新的平台行为或兼容性声明仍要求新证据。玩家桥接、线程归属、协议兼容、可见性或
存档一致性最终仍必须由适当的 sanitizer、平台编译、集成或故障注入测试闭合其 ADR 门禁。
