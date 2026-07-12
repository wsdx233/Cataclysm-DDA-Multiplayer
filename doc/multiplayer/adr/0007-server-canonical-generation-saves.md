# ADR-0007：服务器 canonical generation 存档

- 状态：已接受
- 日期：2026-07-12
- 关联计划：第 5、14、16、19、20 节

## 背景

当前 `game::serialize_json()` 把 turn、bubble 位置、scent、active monsters、global EOC、messages 和唯一 avatar 等不同作用域状态写入同一角色 save 流程。多人服务器若为每名玩家重复写入这些字段，会产生多个互相冲突的世界副本。

服务器权威还要求崩溃后恢复唯一 canonical 世界。多文件保存如果没有 generation 和提交点，会在进程终止、磁盘写满或 rename 失败时留下无法判断的新旧混合状态。当前普通 save 也没有显式持久化 RNG engine 的完整运行状态。

## 决策

多人服务器是世界和所有服务器人物的 canonical save owner，并逐步拆分序列化边界：

- `world_runtime` 保存 calendar、weather、RNG、任务/阵营/事件、全局 ID、bubble/world EOC 和共享世界元数据。
- `bubble_runtime` 保存当前 map、active monsters/NPC、scent、sound/cache 所需的持久状态；v1 只有一个 bubble。
- `player_runtime` 按稳定 `player_id` 保存 avatar、messages、stats/achievements/memorial、map memory、活动和玩家自动化状态。
- 现有单人 save 继续组合一份 world/bubble runtime 与一个 player runtime，保持旧存档读取和 round-trip 兼容。
- 多人客户端不写 canonical 世界 save，也不把本地 save 目录同步给服务器。

保存只能在 turn barrier 的静止点启动。generation 提交流程为：

1. 停止执行新的世界命令，将聊天等非世界消息留在队列。
2. 写入新的 generation 临时目录和 `save_in_progress` 标记。
3. 完成 world、players、地图引用和 manifest 文件的写入、flush 与校验。
4. 所有内容成功后，最后原子更新 `CURRENT` 指向新 generation。
5. 保留最近 N 代；启动时验证当前代，失败则回退上一有效代。

第一阶段可以继续复用现有单文件 temp + atomic rename，但对外的 canonical 语义和最终 generation 提交顺序不得改变。RNG engine 状态必须写入 `world_runtime`，加载时在任何可能调用 RNG 的迁移或对象初始化前恢复。

## 替代方案

- 多个客户端/进程共享 save 目录：拒绝。缺少事务和对象所有权，会造成覆盖、重复实体和地图不一致。
- 每名玩家保存一份完整世界：拒绝。无法定义冲突恢复和唯一 canonical 时间线。
- 只保存初始 seed：拒绝。无法恢复运行中 RNG 调用位置，重启会改变后续结果。
- 首版立即迁移到数据库：暂不采用。不能消除 gameplay object 序列化和 generation 一致性问题，且扩大初期改动。

## 后果

- savegame version、server-state schema 和 portable-character schema 必须分别管理。
- 世界保存失败时必须保留上一有效 generation，不能继续覆盖或宣称成功。
- 地图文件量较大，后续可用 reflink、hardlink 或 copy-on-write 复用未变内容，但 manifest 必须记录实际引用和 hash。
- autosave 由服务器统一计划，不再由每名玩家的本地 `AUTOSAVE` 选项决定。
- 服务器重启和 reconnect 必须从同一 committed generation 恢复玩家 session 与 revision 基线。

## 验证要求

- 老单人 save 加载、保存、再次加载，关键状态 hash 与基线一致。
- 两玩家世界保存/加载后，角色、inventory、item UID、地图、怪物、活动、calendar 和 RNG 后续序列一致。
- 在每个 generation 写入阶段注入失败或 `kill -9`，启动只能选择完整新代或上一有效代，不能加载混合状态。
- 磁盘写满、权限失败、hash 不符和缺失玩家文件均产生可诊断错误且不更新 `CURRENT`。
- 保存期间不会执行改变世界的 command，也不会从工作线程读取 live object。
