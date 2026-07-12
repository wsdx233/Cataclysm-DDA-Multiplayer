# ADR-0006：可见性过滤的远程场景与客户端本地渲染

- 状态：已接受
- 日期：2026-07-12
- 关联计划：第 5、11、19、20 节

## 背景

Windows 与 Android 客户端需要继续使用本地 tileset、字体、语言、缩放、面板和 soundpack。服务器则必须在无 SDL、curses、ImGui 和音频初始化的环境中运行。向客户端发送完整地图会泄露隐藏实体，并把协议绑定到 submap、缓存和 save 内部结构。

像素或视频流无法复用客户端本地表现资源，也不适合回合制 UI、物品列表、角色面板和移动网络环境。

## 决策

服务器为每名玩家生成经过 FOV、map memory 和特殊视觉过滤的语义 `remote_scene`：

- scene 包含 revision、中心绝对坐标、z-level、viewport、ordered sprite command、ASCII fallback cell、overlay、可见生物摘要、光照、天气和语义声音事件。
- sprite command 使用稳定 tile ID、类别、subtile、rotation、layer、光照和可选表现参数，不发送服务端纹理、像素或对象指针。
- 服务器 scene builder 只依赖规则和可见性数据，不依赖 SDL/curses；服务端决定某实体是否可见。
- 客户端新增远程场景渲染入口，并继续通过本地 tileset 解析 tile ID。
- 客户端维护只读的自身 avatar replica，用于角色面板、背包、技能、状态和本地菜单。任何本地修改只能形成 command，不能成为 canonical 结果。
- 消息优先发送稳定 ID 和参数并由客户端翻译；暂时无法结构化的内容可发送标记过的 UTF-8 fallback。
- 声音发送 sound ID、variant、位置、音量和类别，由客户端本地 soundpack 播放。
- v1 不做客户端移动预测；收到服务器确认前不修改 canonical 位置或库存。

## 替代方案

- 同步完整 submap/world：拒绝。泄露隐藏内容并强耦合内部 schema。
- 客户端运行完整平行模拟：拒绝。扩大确定性、作弊、性能和存档兼容问题。
- 服务端视频或 framebuffer streaming：拒绝。失去本地 tileset、语言、面板和可访问性支持，带宽成本过高。
- 只发送 ASCII：可作为 fallback，但不能满足 Windows/Android tiles 客户端的 v1 目标。

## 后果

- server scene builder 与 client renderer 都是新增边界，早期垂直切片可先覆盖 terrain、furniture、player 和 monster，再按 layer 完善。
- viewport 变化可能需要 full scene；稳定区域使用 revision delta 和缓存控制带宽。
- 自身 avatar snapshot 初期可以整体压缩发送，后续再优化字段级 delta。
- 所有可见性过滤必须在服务器完成，客户端裁剪不能作为安全措施。

## 验证要求

- golden test 比较本地规则可见集合与发出的 scene entity/tile 集合。
- 测试确认隐藏怪物、陷阱、未探索地形和不可见物品不出现在 full snapshot 或 delta。
- Windows 与 Android 使用不同本地 tileset/语言连接同一服务器时，语义状态一致且表现可独立配置。
- reconnect、viewport resize、z-level 和 revision gap 能正确触发 delta 或 full scene 恢复。
- 客户端 replica 的本地修改不能绕过 command validation 改变服务器状态。
