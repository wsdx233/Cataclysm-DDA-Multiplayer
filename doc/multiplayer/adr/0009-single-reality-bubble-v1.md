# ADR-0009：v1 单 reality bubble 与距离约束

- 状态：已接受
- 日期：2026-07-12
- 关联计划：第 6、17、18、20 节

## 背景

当前 `game` 只有一份 `map`、`creature_tracker` 和 `scent_map`，声音事件与多种缓存也按单一活动区域组织。立即支持相距任意远的玩家需要多份 map/tracker/scent/sound/cache，并解决 bubble 重叠、合并、拆分、车辆和实体唯一所有权问题。

这些问题与玩家桥接、调度、协议和 headless 都是独立高风险项。把多 bubble 纳入首个可玩版本会显著扩大状态空间，并可能在同一 turn 重复模拟 submap 或实体。

## 决策

v1 只维护一个共享 reality bubble，并对在线玩家设置服务器权威的距离约束：

- bubble 中心根据所有在线玩家的 bounding box 计算，不固定跟随当前活动 `game::u`。
- map 只在 turn boundary shift；shift 必须同步所有 human player、monster、NPC、scent 和 bubble-relative cache。
- 玩家最大 x/y span 必须小于 map 安全范围并保留视距、spawn 和缓存边缘。
- 管理员可配置 tether，默认约 48 tiles。服务器在命令执行前拒绝将玩家带出允许范围的移动、车辆或传送结果。
- 拒绝结果使用稳定、可本地化的错误码，并返回足以刷新客户端场景的 revision/delta。
- 离线角色是否计入中心和 tether 由断线/下线策略明确决定，不能由客户端选择绕过。

多 reality bubble 作为 v1 后的独立阶段。进入该阶段前，`map`、tracker、scent 和 sounds 必须迁入显式 `bubble_runtime`，且每个 submap/entity 在一个 turn 内只有一个 active bubble owner。

## 替代方案

- v1 直接实现多 bubble：拒绝。会把实体所有权、world turn 协调和 merge/split 风险加入首个垂直切片。
- 为每名玩家建立独立 map 副本：拒绝。同一区域会重复模拟并产生冲突世界。
- 玩家越界时静默传送或拉回：拒绝。会破坏行动结果和车辆/战斗状态；应在动作提交时明确拒绝。
- 完全禁止玩家分开：拒绝。共享建筑、街区和战斗区域仍需要合理活动空间。

## 后果

- v1 是同一区域合作玩法，不支持玩家长期分赴不同城镇或 overmap 区域。
- group-centered shift 会触及当前以唯一 avatar 为中心的加载、spawn、声音、气味和可见性逻辑，需要专项审计。
- tether 不是网络延迟补偿，而是明确的产品限制，服务器配置和客户端错误提示都必须公开。
- 后续多 bubble 不得改变全局 calendar 每 turn 只推进一次的共享时间模型。

## 验证要求

- 两到四名玩家在 tether 边缘移动时，合法命令成功，越界命令无部分副作用地失败。
- group center 跨 submap shift 后，所有 human player、monster、NPC、vehicle、scent 和 tracker 索引保持一致。
- 验证地图安全边缘、z-level、车辆乘客、抓取和传送对 span 计算的处理。
- soak test 中不存在重复实体、重复 world phase、玩家落出 map bounds 或 hidden scene 泄露。
- 多 bubble 阶段开始前必须新增替代/扩展 ADR，定义 ownership、merge/split 和全局 turn 协调不变量。
