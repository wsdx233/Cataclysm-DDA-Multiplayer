# ADR-0008：Portable Character 与人物所有权策略

- 状态：已接受
- 日期：2026-07-12
- 关联计划：第 4、14、16、19、20 节

## 背景

产品目标允许客户端选择本地人物加入私服，但现有 `.sav` 不是纯人物文件，其中混有 turn、地图位置、scent、active monsters、全局事件、messages 和其他世界状态。直接上传并反序列化完整 save 会覆盖服务器世界、复制物品和 ID，并把不可信数据送入宽泛的 save loader。

同一个人物在本地世界和服务器世界同时发展后不存在可靠的自动合并规则。物品、伤势、技能、任务、活动和唯一对象都会产生无法判定的冲突。

## 决策

人物跨世界传递使用独立、版本化、白名单式的 `PortableCharacterPackage`，其 schema 与 savegame 和 server-state schema 分开演进。package 至少包含：

- package version、来源 build/content manifest。
- character generation、lease metadata。
- 独立 avatar payload 和可选 cosmetic metadata。
- 完整性/来源签名；私服管理员可以显式允许 unsigned import。

importer 只读取人物白名单字段，不调用完整 `game::unserialize()`。服务器导入时必须：

- 重新分配世界内唯一 `character_id`。
- 递归生成新的 `item_uid`，并重建合法 item reference。
- 由服务器决定出生坐标、dimension 和 world origin。
- 默认清除 missions、followers、camp、zone、NPC/faction representative 和旧世界坐标引用。
- 取消 activity、destination、auto move、grab、mount、remote vehicle、translocator 和其他旧世界控制状态，除非有专门验证。
- 清除 map memory、overmap seen/notes、queued EOC、旧 safe reference 和 debug 状态。
- 按服务器 policy 校验 traits、物品、嵌套 pocket 深度、数量和总包大小。

服务器支持三种明确策略：

- `server_owned`：人物只存在服务器，适合不受信或更严格服务器。
- `portable_lease`：私有合作服务器的推荐默认。客户端签出 generation，服务器持有 canonical copy，正常退出时签回下一 generation。
- `copy_in`：导入副本后本地与服务器永久分叉，只用于明确允许复制的休闲私服。

`portable_lease` 中，本地 checked-out generation 不能继续进入另一个世界；异常断线时 canonical copy 留在服务器，重连继续同一 lease。v1 不提供双向自动 merge。

## 替代方案

- 上传完整 `.sav`：拒绝。包含世界状态和不受控引用，无法安全隔离。
- 始终信任客户端人物字段与 ID：拒绝。允许复制物品、碰撞身份和伪造状态。
- 自动合并本地与服务器分支：拒绝。不存在通用、可验证的冲突规则。
- 只支持 `server_owned`：安全但不满足私服携带本地人物的产品需求，因此保留为策略而非唯一模式。

## 后果

- portable import/export 是独立安全边界，需要专用 parser、schema migration、审计日志和 fuzz target。
- gameplay content manifest 不匹配时必须拒绝导入，不能自动下载或执行未知 mod。
- unsigned import 必须由管理员显式启用，并在客户端清晰标记复制和作弊风险。
- lease 凭据和签名密钥存放在平台私有目录，日志中脱敏。
- 角色死亡、服务器回滚和管理员恢复需要定义 generation/lease 的后续状态，不能静默生成两个有效分支。

## 验证要求

- 单元测试覆盖所有保留、清理和重映射字段，包括深层 nested pockets 和跨物品 reference。
- 重复、陈旧、篡改或错误 content manifest 的 generation 被拒绝。
- 同一 package 重复导入不会在世界中产生重复 `character_id` 或 `item_uid`。
- fuzz 测试覆盖超大字符串、过深物品树、非法 UTF-8、未知字段和截断 package。
- 正常签出/签回、异常断线、服务器重启和 lease resume 不产生可并行使用的 canonical 分支。
