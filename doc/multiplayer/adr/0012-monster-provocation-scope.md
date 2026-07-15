# ADR-0012：怪物玩家挑衅作用域与持久化身份

- 状态：待决策
- 日期：2026-07-15
- 关联计划：第 8、9、20 节与 Phase 3 Gate 3 4A.2a.2

## 背景

上游怪物用已序列化的 monster-wide `aggro_character` 布尔值记录“被角色激怒”。单人模式下只有一个 avatar，
因此这个状态无需回答“由谁触发”或“允许攻击谁”。Gate 3 4A.2a.1 已让静态 `monster::attitude( Character * )`
按 exact human candidate 评估，并在多 living-avatar 的 ordinary planner/basic melee 中拒绝攻击非 HOSTILE human；
它没有改变 `aggro_character` 的含义。

多人模式必须明确：玩家 A 的接近、伤害、幼崽威胁或其他 trigger 将怪物激怒后，是否也立即授权该怪物攻击未参与
挑衅的玩家 B。这个选择会改变 AI、公平性、离线/死亡玩家处理、旧存档迁移和 save schema，不能在 direct-special
适配过程中用临时条件静默决定。

## 当前决策与实现冻结边界

在本 ADR 被接受前，不实现 target-keyed provocation state，不改变 `aggro_character` 的序列化格式，也不宣称
per-target hostility 已关闭。4A.2a.1 继续只提供 candidate-specific instantaneous attitude；其
`living_world_avatar_count() > 1` 分支和 basic `attack_at()` guard 不能外推到 direct specials、forced movement、
projectiles 或 AoE。

4A.2a.2 必须在以下两个候选中选定一个方向，并同步更新重构计划、save migration 与测试矩阵：

1. **保留全局挑衅。** 任一玩家触发 `aggro_character` 后，怪物可把所有满足其他条件的玩家视为被挑衅目标。
2. **按稳定 `character_id` 记录挑衅对象。** 怪物只把已记录玩家视为被挑衅；需要定义集合生命周期、衰减、离线、
   死亡、角色移除和旧存档映射。

## 替代方案

- 在每个 special 中临时检查当前 target：拒绝。不同攻击路径会产生互相矛盾的授权语义，且无法形成可迁移存档。
- 以 runtime pointer、registry index 或连接 ID 标识挑衅者：拒绝。这些身份不稳定，也不能跨 save/restart。
- 把当前 active avatar 当作挑衅者：拒绝。world phase 不得让最后一个 player guard 决定怪物 authority。
- 在 4A.2 中顺带实现完整 per-target anger/morale：不作为本决策门候选。它显著扩大 AI、存档和性能范围；若未来
  需要，必须由独立 ADR/phase 重新提出。

## 后果

- direct special 可以继续按 explicit target、actual hit 和 fail-closed 规则分片审计，但任何依赖“是否已被该玩家
  挑衅”的授权都必须等待本 ADR。
- 旧存档兼容必须显式处理现有 `aggro_character = true`。若采用 target-keyed 状态，需要决定映射为所有当前玩家、
  空集合加 legacy fallback，或一次性迁移标记；不能丢失敌意或无提示扩大敌意。
- 只有 simulation thread 可以读取或修改最终 provocation state；网络线程和客户端不得提交敌意结果。

## 决策与验证要求

- 写出 A 挑衅、B 未挑衅、A/B 分别 active/offline/dead、玩家加入/离开和 save/load 后的期望矩阵。
- 审计所有写入 `aggro_character` 的 trigger 和所有读取 attitude/hostility 的普通攻击与 special 路径。
- 选择 target-keyed 方案时，只能使用稳定 `character_id`，并定义 serialization version、旧存档迁移、去重、清理和
  缺失 registry identity 的 fail-closed 行为。
- 选择全局方案时，必须把“一个玩家的挑衅授权攻击其他玩家”写入产品限制，并用两玩家测试固定该行为。
- 完整 per-target anger/morale 不得作为 4A.2a.2 的隐式扩张；未来重新提出时必须先拆出独立 phase、性能和
  save-schema 计划。
- 接受后以 Linux focused tests 起步，按 ownership/save 风险增加完整 `[multiplayer]` 与 sanitizer；只有接入可达的
  production multi-runtime route 后才增加 process gate。该决策本身不要求日常 Windows/Android package。
