# ADR-0004：语义命令、可见状态与 FlatBuffers 协议

- 状态：已接受
- 日期：2026-07-12
- 关联计划：第 10、11、12、13、19、20 节

## 背景

CDDA 的内部 C++ 对象和 JSON save schema 会随上游持续变化，也包含客户端不应获知的地图、陷阱、怪物和世界引用。直接同步对象内存、完整 submap 或 save JSON 会把网络兼容性绑定到实现细节，同时扩大作弊、崩溃和资源耗尽风险。

项目已经内置 FlatBuffers/FlexBuffers runtime 和 zstd，可以复用现有跨平台依赖，但网络协议仍需要独立 schema、版本和生成流程。

## 决策

网络协议使用版本化 FlatBuffers schema，承载语义命令和经过可见性过滤的状态 DTO：

- 客户端发送 `PlayerCommand` 等 typed message，只描述意图、稳定目标和前置 revision。
- 服务器发送 `CommandResult`、snapshot、delta、choice model、chat 和语义事件，不发送 live object 或完整 save。
- 每个入站 FlatBuffer 先通过 verifier，再执行身份、权限、状态、范围、长度和资源限制校验。
- `.fbs` schema 与生成头文件提交到仓库；CI 使用固定 `flatc` 版本验证生成文件未过期。
- frame envelope 单独包含 magic、协议版本、message type、flags、session、sequence、payload length 和 uncompressed length。
- 小命令不压缩；大 snapshot/delta 可使用 zstd，但必须限制压缩前后大小、压缩比、chunk 数和重组内存。

协议身份只允许稳定值：

- `player_id`、服务器 `character_id`、`item_uid`。
- recipe、spell、terrain、furniture 等稳定字符串 ID。
- absolute map coordinate。
- `entity_ref { kind, stable_id, revision }`。
- 协议 schema 自有的 message/command enum，不复用进程内 `action_id` 数值。

以下兼容性轴分别演进，不共用一个版本号：

- protocol major/minor。
- server-state schema。
- portable-character schema。
- savegame version。
- gameplay content manifest/hash。

每个 command 携带单调递增的 `client_seq` 和 `base_revision`。服务器缓存最近的 command result；相同 session/sequence 的重发返回原结果，不重复执行。delta 必须声明 `base_revision` 与 `new_revision`，缺口通过 full snapshot 恢复。

## 替代方案

- 直接发送 save JSON：拒绝。字段范围过大、兼容轴错误，并允许不可信输入进入 save 反序列化路径。
- 发送 C++ struct、指针或容器下标：拒绝。ABI、生命周期和身份均不稳定。
- 只使用自由格式 JSON RPC：拒绝作为核心协议。虽然便于调试，但难以稳定限制类型、大小和生成代码一致性。
- 每回合发送完整 submap/world：拒绝。泄露隐藏状态，带宽和移动端内存成本不可接受。

## 后果

- 每个新增远程动作都需要 schema、服务器验证、客户端 UI、错误码和重连测试，不能只增加一个 socket handler。
- 协议 minor 兼容必须通过 capability negotiation 明确声明，不能假设旧客户端可忽略任意字段。
- FlatBuffers verifier 不能替代业务校验；UTF-8、嵌套深度、数量、速率和权限仍需独立检查。
- 调试工具需要能解码 frame 和打印脱敏后的语义内容，日志不能记录 token 或完整人物包。

## 验证要求

- frame codec 测试覆盖单字节分片、粘包、空 payload、超长、截断和未知 message type。
- schema 测试覆盖 major/minor negotiation、未知 capability、generated header 一致性和恶意 buffer。
- 幂等测试覆盖“服务器已执行但确认丢失”后的重连 replay。
- visibility golden test 证明协议 payload 不包含玩家不可见的怪物、陷阱、物品和未探索地图。
- fuzz target 覆盖 frame decoder、FlatBuffer root、zstd envelope 和 UTF-8 字段。
