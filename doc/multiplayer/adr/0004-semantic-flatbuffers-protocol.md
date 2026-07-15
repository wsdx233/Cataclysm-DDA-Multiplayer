# ADR-0004：语义命令、可见状态与 FlatBuffers 协议

- 状态：已接受
- 日期：2026-07-12
- 更新：2026-07-15
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

握手中的 `build_id` 是独立于显示版本的规范源码身份：

- 格式必须是 40 位小写 Git SHA，可选追加 `-dirty`，即 `^[0-9a-f]{40}(-dirty)?$`。
- Make、CMake、Gradle 和 MSVC 对同一 source tree 必须生成相同值；CI 可以显式注入构建 commit。
- UI/backend suffix（例如 `+SDL3`）、tiles/sound capability、generator tag、包名和显示 `VERSION` 不参与该身份。
- server 或 client 无法取得合法 ID 时 multiplayer 启动 fail closed；不同 ID 的握手拒绝，同源的不同 backend
  artifact 必须兼容。
- 已有生成头不能成为跳过 identity 重算的依据；CMake 等实际 build target 必须重新运行无副作用生成器并传递显式
  override。Git dirty-state probe 只有 rc 0/1 分别表示 clean/dirty，rc > 1 必须令生成失败，不能伪装成 `-dirty`。
- CI 对 canonical ID 的 artifact/`--version` 文本检查必须区分大小写；小写格式要求不能只依赖 PowerShell 默认的
  大小写不敏感比较。

每个 command 携带单调递增的 `client_seq` 和 `base_revision`。服务器缓存最近的 command result；相同 session/sequence 的重发返回原结果，不重复执行。delta 必须声明 `base_revision` 与 `new_revision`，缺口通过 full snapshot 恢复。

protocol minor `1` 起，`ResumeRequest` 还必须携带客户端最后接受的 `session_generation`。服务器将它与 bearer
token record 和 authoritative runtime 交叉验证：正常 resume 只提交精确 `old + 1`；若上一 accepted resume response
在客户端确认前丢失，只有 generation、last revision 和 last client sequence 与上一 committed resume fingerprint
全部匹配时，才能在新 transport session 上幂等重发同一 generation。该字段不能作为客户端自报 authority，也不能
允许跳代、回退或绕过 token。新 session 的第一条有效 application frame 会触发 exact-tuple simulation confirmation；
只有 authoritative directory 接受 confirmation 且 server/main ack lobby mirror 后才完整消费该 fingerprint，之后旧
generation 不能继续冒充 response-lost retry。

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
- 所有生产构建路径必须生成并验证 canonical multiplayer build ID；`--version`、server structured log 和 artifact
  provenance 应暴露它，便于核对跨平台 artifact 是否来自同一 source identity。
- 生成步骤可以每次 build 调用，但必须使用 content-aware write，未变化时不能无故改写 `version.h` 并触发全量重编。

## 验证要求

- frame codec 测试覆盖单字节分片、粘包、空 payload、超长、截断和未知 message type。
- schema 测试覆盖 major/minor negotiation、未知 capability、generated header 一致性和恶意 buffer。
- build identity 测试覆盖合法 full SHA、可选 `-dirty`、空值/非法值 fail closed、不同 ID 拒绝，以及同源
  Linux curses、Windows SDL3 和 Android SDL3 artifact 使用同一值。
- build-system 回归覆盖已有 `version.h` 时的重新核对、CMake override 贯穿 target、重复生成保持 mtime、Make
  dirty probe rc > 1 fail closed，以及 Windows workflow 的大小写敏感 canonical-ID assertion。
- 幂等测试覆盖“服务器已执行但确认丢失”后的重连 replay。
- resume admission 测试覆盖 accepted response 丢失后的同代 completion replay、客户端已接受后下一次精确 `+1`、
  跳代/回退拒绝和旧 connection/session/generation tuple 不覆盖新绑定。
- visibility golden test 证明协议 payload 不包含玩家不可见的怪物、陷阱、物品和未探索地图。
- fuzz target 覆盖 frame decoder、FlatBuffer root、zstd envelope 和 UTF-8 字段。

## 已记录验证

- `e078eb6aef25a9cc72eea45793114c931a52b896` 的 baseline run `29328086046` 与 transport/protocol run
  `29328086326` 均为 terminal `success`，证明同源 Linux、Windows MSVC、Android artifacts 使用 canonical ID；
  Android KVM auth/render/wait/move/lifecycle smoke 精确归属于该提交。
- 只读审计后的 hardening commit `86336ea847bea45f727fd97d74a811a32712518c` 由 baseline run
  `29330811779`（7 jobs）和 transport/protocol run `29330811746`（3 jobs）终态全绿验证最终源码的 Linux、
  Windows、Android build/test gates，以及 Windows 大小写敏感 artifact assertion。顶层 CMake
  always-run/override 与 Make dirty-probe fail-closed contract 另由本地 Ninja、Unix Makefiles 和故障注入回归直接验证。
