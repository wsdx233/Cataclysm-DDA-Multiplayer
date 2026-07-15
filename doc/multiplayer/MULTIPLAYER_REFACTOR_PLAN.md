# CDDA 联机版最小侵入式重构与实施计划

## 1. 文档信息

- 上游仓库：`https://github.com/cleverraven/cataclysm-dda`
- 调研基线：`d84b90d` (`cdda-experimental-2026-07-11-0744`)
- 基线日期：2026-07-11
- 克隆方式：`--depth 1 --single-branch --filter=blob:none`
- 目标平台：Android 图形客户端、Windows 图形客户端、Linux/Windows 无界面服务器
- 目标连接方式：客户端输入服务器地址后直连
- 文档性质：长期 fork 的架构与交付计划，不代表 CDDA 上游项目路线
- 架构决策记录：[ADR 索引](adr/README.md)

本文将需求中的“跨段联机”按“跨端、跨平台联机”理解，也就是 Android 与 Windows 客户端可以连接同一个 headless 服务器。如果这里同时指“玩家能够跨地图分区、相距任意远仍在线”，则对应本文后面的“多现实泡”阶段，不应作为首个可玩版本的前置条件。

## 2. 执行结论

推荐的总体方案如下：

1. 服务器权威：世界、角色、随机数、行动结果、存档均由服务器决定。
2. 客户端保留本地体验：按键、触控快捷键、tileset、字体、语言、面板、音效和窗口布局继续读取客户端本地设置。
3. 网络上传输语义命令和可见场景，不同步整个存档，不把客户端计算结果当作事实。
4. 模拟仍保持单线程：网络线程只能收发、校验和排队，任何世界修改只能发生在模拟线程。
5. 时间采用共享回合屏障：所有在线玩家完成当前秒内可用 moves 后，怪物、NPC、天气、场和物品等世界阶段才推进。
6. 首版只维护一个共享 reality bubble，并限制玩家分离范围。这能复用当前 `map`、怪物列表、NPC 列表、声音和气味系统，显著减少改动。
7. 每名玩家的完整 avatar 使用地址稳定的 registry ownership；`game` 只切换活动玩家上下文，让 `get_avatar()` 等旧入口转发到当前执行者。Phase 0 已证明完整 avatar move-swap 会破坏持久物品引用，因此不按原样采用；sidecar 只用于隔离 player-scoped 状态，不再作为移动 Character 主体的默认退路。
8. 将 `game::handle_action()` 拆成“本地输入解析”和“服务器命令执行”，单人模式也走同一个命令执行器，避免维护两套游戏规则。
9. 客户端人物不能直接把完整 `.sav` 上传。新增版本化的 portable character package，并对坐标、任务、ID、活动、世界引用和物品 UID 做清理及重映射。
10. 多现实泡、自动 NAT 穿透、公共大厅、任意版本兼容和自动分发 mod 均后置。

这不是一个“增加 socket”级别的功能。可靠的首个联机垂直切片预计需要 2 至 3 名熟悉 CDDA/C++ 的开发者投入 3 至 5 个月；接近完整单人功能覆盖的稳定 v1 预计需要 9 至 15 个月。单人全职开发通常应按 18 至 30 个月规划。支持玩家任意距离分离的多现实泡还会额外增加约 4 至 8 个月。

## 3. 当前代码事实与约束

### 3.1 已确认的核心耦合

当前主循环位于：

- `src/main.cpp`：创建唯一 `game`，进入 `while( !g->do_turn() )`。
- `src/do_turn.cpp`：`game::do_turn()` 同时负责时间推进、玩家输入、活动、怪物/NPC、天气、地图处理、UI 刷新和音效。
- `src/game.h`：`game` 固定持有一个 `map`、一个 `avatar`、一个 `scent_map` 和一个 `creature_tracker`。
- `src/game.cpp`：`get_avatar()`、`get_player_character()`、`reality_bubble()` 都直接返回唯一对象。
- `src/handle_action.cpp`：`game::handle_action()` 获取本地输入并调用 `do_regular_action()`；大量动作在执行过程中直接打开阻塞 UI。
- `src/messages.cpp`：消息队列是进程级单例，而不是按玩家隔离。
- `src/sounds.cpp`：声音事件与声音标记也是进程级静态容器。

在当前基线上，包含以下调用的源文件数量约为：

| 调用或模式 | 涉及源文件数 |
| --- | ---: |
| `get_avatar()` | 107 |
| `get_player_character()` | 119 |
| `get_map()` | 153 |
| `query_yn()` | 65 |
| `uilist` | 100 |

因此，一次性把所有函数都改成显式传入 `player_context` 会制造极大的 diff，也会让后续合并上游变得困难。本计划采用兼容层先收口热点，然后按实际需要逐步消除全局访问。

### 3.2 可以复用的现有基础

以下现有结构对最小改动方案有直接价值：

- `Character` 是 `avatar` 与 `npc` 的共同基类。
- `avatar::control_npc()` 已通过 `Character::swap_character()` 在主角与 NPC 之间交换角色主体状态。
- `avatar` 已有默认 move constructor 和 move assignment；Phase 0 对照测试证明其序列化值可完成 move-swap round-trip，同时也证明持久运行时引用不会自动随值迁移。
- `game::do_regular_action()` 已经是输入解析之后的主要动作分发点。
- `current_map` 与 `swap_map` 已提供 RAII 切换当前地图的先例。
- Phase 0 已实现地址稳定的 `multiplayer_player_registry`/`multiplayer_player_runtime`、
  `multiplayer_active_player_guard` 和额外 human tracker/query/map-shift 支持，并有 identity/sanitizer 正向矩阵。
  这些是 Phase 3 要接入 production scheduler/session 的既有基础，不应再复制一套 registry/context。
- `map.h` 已明确备注未来若存在多个 reality bubble，需要拆分 active bubble 与 all bubbles 语义。
- 角色文件、`master.gsav`、dimension data、地图/overmap 数据已经分文件保存。
- `worldfactory::make_new_world(name, mods)` 已支持无 UI 创建世界元数据。
- 项目已内置 FlatBuffers/FlexBuffers 与 zstd，可复用协议编码和压缩能力。
- Android 与 Windows 图形构建已经存在，客户端不需要另起技术栈。

### 3.3 不能误用的现有功能

`MAP_SHARING` 只是共享目录/用户名/权限相关的旧模式，不是联网、并发控制或世界同步实现。不能把多个 CDDA 进程直接指向同一个 save 目录，这会造成并发覆盖、对象重复、地图文件不一致和不可恢复的存档损坏。

上游文档 `doc/FREQUENTLY_MADE_SUGGESTIONS.md` 明确表示不接受 multiplayer，理由包括长活动时间、阻塞交互、同步复杂度和安全问题。因此应按长期 fork 管理，不应假设 multiplayer 专用改动能合入上游。

## 4. 产品范围

### 4.1 v1 必须交付

- Android 和 Windows 图形客户端连接同一个 headless 服务器。
- 服务器可通过配置文件创建或加载世界。
- 客户端从本地人物列表选择人物，也可为该服务器创建新人物。
- 客户端视角与单人游戏一致，使用本地 tileset、字体、语言、面板、键位和 Android 触控快捷键。
- 至少支持 2 至 4 名玩家同处一个 reality bubble。
- 玩家能看见、阻挡、帮助、攻击其他玩家，并与同一世界中的怪物、NPC、物品、地形和车辆交互。
- 服务器保存所有在线/离线玩家和世界状态。
- 断线后能够在宽限期内恢复同一会话；服务器重启后能够恢复人物。
- 基础文本聊天。
- 严格版本、数据和 mod 兼容检查。
- 至少覆盖移动、等待、近战、开关门、拾取/丢弃、穿戴、使用、进食、阅读、装填、射击、制作、睡眠和 NPC 对话等主流程。
- 单人模式行为与旧存档兼容不得明显退化。

### 4.2 v1 明确不做

- 公共服务器浏览器和中心账号系统。
- 内置语音。
- 自动 NAT 穿透、官方中继和全球匹配。首版只支持局域网、端口映射、IPv6 或外部 VPN。
- 自动从服务器下载未知 mod、原生库或可执行内容。
- 不同游戏提交、不同 savegame schema 或不同 gameplay data 混连。
- MMO 规模、数十人同图、数据库化世界。
- 完整竞技反作弊。服务器权威可以阻止伪造行动结果，但“携带客户端人物”本身需要服务器策略决定信任级别。
- curses 联机客户端。首版优先 Android/Windows tiles 客户端。
- 玩家任意距离分离。首版采用单 reality bubble 距离约束。

## 5. 架构原则

### 5.1 权威边界

服务器拥有并决定：

- 世界时间与 RNG 状态。
- 地图、overmap、怪物、NPC、车辆、天气、场、物品和任务。
- 所有角色的真实属性、位置、moves、活动和库存。
- 行动是否合法及其最终结果。
- 可见性、听觉结果和客户端可以获知的世界切片。
- 世界与角色的 canonical save。

客户端拥有并决定：

- 物理输入到本地 action 的映射。
- tileset、字体、缩放、窗口、面板、颜色、语言和音效包。
- Android 触控快捷键及本地 UI 布局。
- 菜单中的本地浏览状态、筛选、排序和光标位置。
- 对服务器状态的只读镜像和渲染缓存。

客户端不能决定：

- 自己的新位置、伤害结果、消耗品数量、制作结果或 RNG。
- 某目标是否可见、可达或可交互。
- 服务器世界选项和 mod。
- 其他玩家或隐藏实体的状态。

### 5.2 线程模型

采用以下固定规则：

1. 一个模拟线程拥有所有 CDDA 世界对象。
2. 一个网络 I/O 线程运行连接和 socket 状态机；必要时可增加一个压缩工作线程。
3. 网络线程只生成不可变 DTO 并放入有界队列。
4. 模拟线程在明确的安全点取走命令并修改世界。
5. 发送快照前，模拟线程先构建不可变 snapshot DTO；压缩线程不能读取 live game object。
6. 保存操作只在 turn barrier 完成后的静止点开始。
7. 禁止用“每个玩家一个线程”执行 `game` 方法。当前全局状态、缓存和静态容器不具备线程安全性。

### 5.3 依赖方向

目标依赖方向：

```text
Android/Windows client
  local input -> command builder -> transport
  local UI <- client replica <- snapshots/deltas/events
  local renderer <- visible scene + local tileset

                         TCP/TLS

Headless server
  transport -> bounded command queue -> simulation thread
  simulation thread -> player scheduler -> existing game rules
  simulation thread -> visibility-filtered snapshot builder -> transport
  simulation thread -> server save manager -> world/player saves
```

网络层不得依赖 SDL、curses 或具体菜单。游戏规则不得依赖 socket。客户端渲染不得持有服务器内部对象指针。

## 6. 状态作用域重构

在写网络代码之前，必须先为状态确定作用域。目标分类如下：

| 作用域 | 典型状态 | 当前问题 | 目标归属 |
| --- | --- | --- | --- |
| World | calendar、weather、missions、factions、timed events、全局 ID、RNG、overmap | 混在 `game` 与玩家存档中 | `world_runtime` |
| Bubble | `map`、active monsters/NPCs、scent、sound events、light/cache | 固定只有一份 | v1 为单一 `bubble_runtime`；后续由 manager 管理多份 |
| Player | avatar、messages、safe mode、map memory、stats、achievements、activity、remote vehicle cache | 大量放在 `game` 或全局单例 | `player_runtime` |
| Client only | input manager、tileset、font、panel、language、sound、window、touch shortcuts | 服务器不应初始化 | 仅客户端进程 |

### 6.1 首批必须迁出的 player state

优先从 `game` 或静态单例迁入 `player_runtime`：

- `uquit`
- `safe_mode`
- `mostseen`
- `turnssincelastmon`
- `auto_travel_mode`
- `destination_preview`
- `driving_view_offset`
- `remoteveh_cache` 与 cache time
- 消息队列和已读位置
- 每玩家 stats/achievements/memorial
- 每玩家自动拾取、安全模式、自动笔记等规则引用
- 每玩家 map memory 与 overmap seen/note 视图
- 每玩家 UI 等待状态中会影响规则的部分

不要求第一步删除旧字段。可以先让 `game` 的旧访问方法转发到当前 `player_runtime`，保持单人调用点不变。

### 6.2 `active_player_guard`

Phase 0 已新增只允许模拟线程使用的 `multiplayer_active_player_guard`，并验证稳定 owner、嵌套恢复和 identity
不变量。以下接口表示其长期职责；Phase 3 的任务是通过 production scheduler/session directory 使用现有 guard，
而不是另建同类上下文：

```cpp
class active_player_guard {
  public:
    active_player_guard( game &g, player_id id, activation_reason reason );
    ~active_player_guard();
};
```

进入时完成：

- 将活动 avatar/runtime 指针切换到 registry 中地址稳定的目标玩家，不移动 avatar 或 Character 值。
- 切换消息 sink、stats、achievements、safe mode 等玩家状态。
- 确保 `get_avatar()`、`get_player_character()` 和相关兼容入口指向目标玩家；迁移期固定 `game::u` 只作为单人 backing storage，不能代表多人活动身份。
- 设置调试上下文，记录哪个玩家、哪个命令正在执行。

退出时完成：

- 按严格 LIFO 恢复上一个活动指针和 player-scoped 上下文。
- 检查 avatar/runtime 地址、角色 ID、位置、item UID、shared ownership 和 tracker 索引不变量。

任何网络线程调用该 guard 都应触发断言。

Phase 3 的 `multiplayer_turn_phase_adapter` source seam 已把 guard 的最低运行合约变成可测试接口：只允许模拟线程，
要求 scheduler current slot、`player_id`、session generation、registry ownership、runtime lifecycle 和构造时记录的
root active context 全部精确匹配；guard 必须明确报告 engaged，退出后必须恢复同一 root context。该 adapter 当前只
接受 `active` runtime，因此 production session directory 不能在断线一发生就把 runtime 置为 `offline`：transport
断开、barrier 内断开和 runtime offline 是三个不同状态，必须先在仍可激活的稳定 owner 上完成权威自动 wait。

## 7. 最小侵入式多玩家角色模型

### 7.1 首选方案：稳定 avatar 地址加活动上下文指针

`player_registry` 以 `unique_ptr`、真实 shared ownership 或其他地址稳定方式持有每名玩家的完整 `player_runtime` 和 `avatar`。玩家加入世界后，活动玩家切换、registry 扩容和连接状态变化都不得移动 avatar 对象。

切换玩家时：

1. scheduler 按稳定 `player_id` 从 registry 解析目标 runtime。
2. `active_player_guard` 保存上一个上下文，并把 `game` 的活动 avatar/runtime 指针改为目标对象。
3. guard 在整个激活区间持有 registry/runtime 提供的 shared owner；活动裸指针必须等于该 owner 的 `.get()`，`shared_from()` 必须返回同一控制块。
4. `get_avatar()`、`get_player_character()`、消息和 stats 等兼容入口在 guard 期间转发到目标 runtime。
5. 执行现有 avatar 动作；短期裸引用不得跨出本次 command safe point。
6. guard 退出时恢复上一个上下文，不复制、移动或写回 avatar 主体。

优点：

- 玩家身份、avatar 地址和 character-backed `item_location` owner 保持一致。
- inventory、worn、wielded、activity、map memory、任务、配方和日记无需在每次命令前后搬运。
- 现有 getter 调用点可以保持不变；直接使用固定 `game::u` 的内部路径由编译器和 phase audit 分批迁移。
- 非活动玩家仍是 human avatar，不运行 NPC AI。

持久引用规则：

- 跨 command、turn、disconnect 或 save/load 的角色引用使用 `player_id`/`character_id` 与 generation。
- 物品引用使用 `item_uid`、owner/location context 和 revision；指针与 safe reference 只是可验证缓存。
- 缓存 owner ID、UID 或 generation 不匹配时必须丢弃并重新解析，不能信任旧地址。
- 当前活动 avatar 使用 registry 对真实 avatar/runtime 的 shared ownership；legacy `u_shared_ptr` 的 null-deleter alias 仅允许表示 `game` 自身拥有的单人 backing avatar，不能用于额外 human player。

### 7.2 已排除的原样方案：完整 avatar move-swap

Phase 0 GCC spike 已证明两个 avatar 的序列化值可以连续 move-swap 10,000 次，但 persistent reference 不变量失败：

- 预先保存的 wielded `item_location` 在 swap 后失效。
- inventory `item_location` 的 cached `Character *` 仍指向旧槽，carrier ID 变成另一玩家。

因此不继续通过逐项 swap 后修复补丁扩展该方案。测试保留为失败对照，防止未来再次把“状态 hash 相同”误当成“运行时身份正确”。

### 7.3 Player runtime sidecar 的保留用途

sidecar 继续用于把当前散落在 `game` 或进程级单例中的玩家状态迁入 `player_runtime`，例如 messages、safe mode、stats、achievements、memorial、自动化规则和 connection/session 状态。

不把 `Character::swap_character()` 加 sidecar 作为默认玩家激活机制，因为该操作仍移动 weapon、inventory、worn 和 activity，不能从根本上保证引用地址稳定。只有后续证据证明某个受限主体交换边界完整且必要时，才以独立 ADR 重新评估。

### 7.4 Session directory 与角色 registry 分工

- 稳定的 `player_id`，使用随机 UUID，不复用 `character_id` 作为账号 ID。
- 服务器分配且世界内唯一的 `character_id`。
- session directory 只在 simulation thread 改变 connection/admission、权威 generation、barrier participation 和
  runtime lifecycle；registry 持有地址稳定 runtime/avatar，并提供这些状态的只读镜像与身份查询，不能独立推进
  session transition。
- 当前连接、角色状态、所在 bubble、最后确认 revision 的可查询 snapshot。
- 一个只由 session directory 推进的 canonical session generation；runtime 是 directory-controlled mirror，lobby
  token record 是 wire/replay mirror，scheduler participant key 是 turn-local snapshot，三者都不得独立递增。
  accepted resume 已 commit 但 response enqueue 失败时，lobby mirror 可以按明确定义暂时落后 directory/runtime
  一代；该差异只能由下一次 exact-fingerprint directory replay 修复，不能成为第二个 generation owner。
- auth/resume 必须采用两阶段提交：network lobby 只完成 wire validation、rate limit 和 token lookup，产生 pending
  request；simulation thread 的 session directory 先生成带 version 的 plan，lobby 按 plan 预编码 response，accepted
  plan 再提交 identity/runtime/generation/connection tuple。随后 transport 先 enqueue response（rejection 使用单个
  ordered `send_and_disconnect`），lobby 最后发布 token mirror 与 post-commit event。不得由 lobby 先发送成功、递增
  record，再让 simulation 侧被动追认。accepted enqueue 失败时 generation 不回滚且只清 binding：resume 保留旧
  token mirror，由 directory fingerprint 在重试时同代 replay 并修复 mirror；fresh auth 尚未发布 token record，后续
  fresh retry 是新的 admission 并继续推进 generation，不承诺同代 replay。
- `ResumeRequest` 携带客户端最后接受的 generation、token、revision 和 client sequence。lobby 先以 token record
  校验 wire request 并形成 immutable pending DTO；directory 再以该 DTO 核对 authoritative entry/runtime 和
  fingerprint。两层都不能信任客户端单独声明，但 directory 不直接读取 network-thread token map。该字段解决
  accepted resume response 丢失时无法判断“重放已提交 `g + 1`”还是“推进到 `g + 2`”的歧义，因此 protocol minor
  随之递增并按公共 wire boundary 验证。
- 普通 resume 只允许 exact `old -> old + 1`。若 directory 已提交 `old + 1` 但 response 未被客户端确认，只有
  connection 已断开、request 仍携带 old generation，且 last revision/client sequence 与上一 committed resume
  fingerprint 精确匹配时，才允许在新 transport session 幂等重放同一 generation；不得再次递增。其他回退、跳代
  或旧 fingerprint 一律过期。
- 新 session 的第一条有效 application frame 触发 exact-tuple confirmation；只有 simulation-thread directory 接受
  confirmation 后，authoritative replay fingerprint 才视为消费，server/main 再显式 ack lobby 完成对应 mirror
  状态。lobby 在 ack 前只能标记 confirmation pending，不能提前清 fingerprint；确认或 ack 异常必须 fail-stop，不能
  再次推进 generation。fresh auth token 在客户端 application confirmation 前若连接先断开，应
  删除未知 token record；会令客户端清 token 的终态 `session_expired` 也必须释放 inactive record，
  active/pending 冲突则保留 token 并返回 `invalid_state`。
- runtime 需要 directory-only 的 exact generation transition（`expected_old -> new`，且只允许 `+1`）；现有
  `begin_session()` 的隐式自增和 active/offline 限制不能直接承担跨 lobby/runtime 的权威提交。canonical generation
  必须小于 `INT64_MAX`（与 player snapshot 可反序列化范围一致），lobby、runtime、scheduler 和 client accepted-resume
  检查统一拒绝更大值；client 必须验证 response 恰好是上一 generation 的 `+1`，不能只验证“变大”。
- 进程启动时 directory 可以一次性采用 registry 已激活 root runtime 的现有有效 generation 作为 bootstrap；一旦建立
  entry，后续 auth/resume 都必须通过 directory version 与 exact transition，不能再次静默 adopt。
- 分离 transport connection state、当前 barrier participant state 和 runtime lifecycle state；不得用单一
  connected/offline 布尔值同时驱动网络关闭、自动 wait 和 avatar 可激活性。
- active/offline/dead/importing 等状态机。
- 地址稳定的 avatar/runtime ownership；容器 rehash 或扩容不得移动 live avatar。
- `creature_at()`、`all_creatures()`、`shared_from()`、`critter_by_id()` 对 human players 的支持。
- 地图 shift 时同步移动所有玩家的 bubble 坐标。
- 同一 tile 的骑乘等合法叠放规则，其他情况拒绝重复占位。

Phase 0 最初用 `character_id` 加绝对坐标的按查询刷新索引验证稳定地址和查询边界；随后已经替换为包含
RFC 4122 v4 `player_id`、session generation、状态机和严格 snapshot schema 的 `player_runtime`
ownership。普通移动由 registry 在查询边界验证位置快照，`map::shift()` 显式同步全部 human runtime 的
位置、route 和 remote-control 坐标；同格查询优先稳定 human identity，human avatar 不进入 NPC AI。
这些是进 Phase 1 的运行时身份基础，但仍不是网络协议或 canonical generation world save。
Phase 3 的 production single-player path 已接入 authoritative session directory、两阶段 admission 与 canonical
generation owner；lobby 只保留受控 token/replay mirror，旧 connection/session/generation tuple 不能覆盖新 binding。
Gate 1 source `cbd19b48d652be735a3c83fe841d2d7c831aecba` 补齐 pure selected-root lifecycle contract 和 exact
directory/runtime/scheduler transition；Gate 2 source `2e9236c7bf91782ad3f15daa5d8aa0e3929f355a` 进一步由
`multiplayer_single_root_owner` 私有组合 directory、scheduler、lifecycle 与 phase adapter，并把 admission publish、
disconnect/grace、forced wait、actual world claim、player-end completion、offline/dormant、same-runtime resume、typed
fatal/no-save 和 graceful transport result 接入 production server。recipe 仍不是 directory commit receipt，所有
external effect 与 lifecycle record 必须留在同一 owner transaction 中。ADR-0010/0011 已按该 source 的 Linux
full/sanitizer/process 证据接受；`players.max` 继续为 `1`，直到 Gate 3 的 two-runtime owner 与规则矩阵关闭。

## 8. 回合与时间模型

### 8.1 选择共享回合屏障

不采用客户端 lockstep，也不让世界按真实时间持续流逝。推荐模型：

1. 世界开始一个 1 秒 turn。
2. 为所有在线玩家运行 turn-begin 处理并准备 moves。
3. 按稳定、轮换的顺序，每次从一名玩家取一个命令执行。
4. 玩家仍有 moves 但没有命令时，服务器停在 barrier 等待该玩家。
5. 玩家进入可自动推进的 activity 时，服务器自动执行 activity step，不要求每秒手动确认。
6. 所有参与 barrier 的玩家 moves 耗尽、明确 wait，或由 policy 自动 wait 后，执行地图、怪物、NPC、天气、场、物品和 world-end 阶段。
7. 发布新 revision 和每玩家快照。
8. 进入下一秒。

这最接近当前 `game::do_turn()` 的“玩家先花 moves，随后世界和怪物行动”结构。

Phase 3 已实现纯 `multiplayer_turn_scheduler` policy、`multiplayer_turn_phase_adapter` 和 production
`multiplayer_single_root_owner`。scheduler 固定以下边界：

- scheduler 不可复制或移动，避免复制 barrier/world claim 状态；`begin_turn()` 复制最多四人的 immutable roster
  snapshot，中途 join 只能进入下一 turn。
- `player_id` 是稳定 roster identity；session generation 是每次 barrier 转换校验的可变元数据，resume 只接受
  精确旧 generation 的 `+1`。
- typed `accepted_remains_eligible` 和 `accepted_finished` 推进 cursor；`rejected`、`duplicate` 保持 current slot、
  round 和 ordering 不变。
- disconnected timeout 只可处理 current slot。`automatic_wait` 与 `remove_from_barrier` 先进入
  `automatic_wait_pending`，不得直接越过权威规则执行。
- 所有 snapshot 参与者 terminal 后进入 `world_ready`；claim 返回执行当前 shared-turn world phase 的 permission
  marker 并转换到 `world_processing`，由 `record_world_completed()` 记录匹配 turn 后才可开始下一 turn。

phase adapter 已把纯状态 API 包装为以下不可交换顺序：验证 exact slot/generation/owner/root context，在目标
`multiplayer_active_player_guard` 内先执行共享规则，再对 accepted action 完成目标 avatar 的 action bookkeeping，
退出 guard 并验证恢复，最后才记录 scheduler transition。权威自动 wait 使用显式 forced mode，绕过玩家请求的
safe-mode gate，但仍执行真实 `Character::pause()`；safe-mode 查询也已改为读取当前 active avatar，而不是固定
`game::u`。adapter 与 scheduler 都不可复制/移动，任何 callback 异常、已发生副作用后的 record failure 或 context
恢复失败都会永久 latch scheduler execution fault，阻止新 adapter 或新 turn 重试非幂等工作。

Gate 1 的 `multiplayer_selected_root_lifecycle` 仍是不拥有 socket/runtime/scheduler/game object 的顺序契约；Gate 2
owner 已成为这些 external effects 的唯一 production 编排者。dedicated server 的 semantic command 与 forced wait
经 adapter 执行，actual legacy bubble 只能在 exact world claim 内调用，scheduler world completion 位于 world hook
内部，而 lifecycle turn completion 必须等 owned turn 的 player-end 返回后才记录。unbound/dormant admission 停留在
outer pump；open player-input barrier 仅允许 ADR-0011 定义的 bounded active-turn pump。scene/save/next turn 都位于
completed lifecycle boundary 之后；open-turn stop 或不可证明的 post-side-effect fault 使用 typed fatal/no-save。
这已关闭 single-root production lifecycle，但尚未证明 two-runtime production routing 或完整多人规则，因此
`players.max` 保持 `1`。

Gate 3 source `2eccb92087991966423c18b63f6ef707462b1428` 新增独立的
`multiplayer_multi_runtime_barrier_owner` inner contract：它在任何 scheduler mutation 前验证完整 immutable roster，
pin exact runtime/avatar shared owner，并在 exact action、zero-action terminal、forced wait 和 world 前复核同一 owner
identity；一个 persistent scheduler 跨 turn 保留公平首位轮换。scheduler world 成功后还必须经过显式
`player_end_pending` 和 exact process-local receipt 才能开始下一 turn。该 owner 尚未组合 transport/session directory、
selected-root 切换、actual legacy bubble、outer lifecycle 或 canonical save，因此不改变 production single-root 路径。

Gate 3 source `3204f8f45606a20ea6ab0892369b9806f89c4353` 在该 inner owner 与既有 semantic executor 之间增加 thin
`multiplayer_multi_runtime_command_router`。router 不拥有 transport、admission、revision 或 dedup，只允许 owner 已验证
的 exact current participant 在 active-player guard 内调用 `multiplayer_execute_basic_command()`；accepted execution
按目标 avatar 执行后的 moves 映射为 `accepted_remains_eligible`/`accepted_finished`，无 action 的
rejected/duplicate 不推进 cursor，不一致的 result/action 组合返回无 disposition 并沿 adapter 既有 fault path
fail-stop。multi-runtime move 暂时 fail-closed 为已审计的同层相邻空地子集，其他 legacy movement branch 在规则执行前
拒绝。这是可逐步扩展的 rule boundary，不是 production command/session owner，也不放宽 `players.max = 1`。

Gate 3 source `84d8ca056bf72ed9776890f7ca4212a3bfdf7c2e` 在同一 inner router 中关闭 adjacent semantic move 的 human collision
authority。registry 用 exact absolute position 查询并排除 mover；命中另一 human 时，router 在 legacy movement callback
之前返回 typed `blocked_by_player` 与 blocker 的 exact participant key。该结果映射为 `rejected/invalid_state`、零 moves、
零 action bookkeeping 和 scheduler `rejected`，因此 current slot 保持不变，也不会隐式交换位置或解释为 PvP attack。
若 registry occupant 无法解析 matching runtime，router 不猜测结果而沿既有 no-disposition fail-stop。blocker key 只属于
inner authority；未来 outer wire owner 必须先做 visibility filtering，不能直接公开 session generation 或不可见玩家
身份。本 slice 不改变 production single-root owner、wire schema 或 `players.max = 1`。

### 8.2 公平顺序

- 每个 turn 只执行每名玩家一个命令，然后轮到下一名仍有 moves 的玩家。
- 首位玩家按稳定 `player_id` 轮换；当前 policy 选择上个已完成 turn 首位的字典序后继并在末尾回绕，避免 host、
  先加入者或 roster churn 造成永久冲突优先权。
- current slot 没有命令时必须保持稳定；rejected/duplicate 结果不消耗轮次，accepted 结果才推进。
- owner-level basic rule matrix 必须验证 exact `first move -> second move -> first wait -> second wait`，并证明每次只
  修改目标 avatar；non-current、malformed 与 unsupported move 不消费当前 slot。
- 两玩家争同一 empty tile 时，ordering-first command 成功，后执行者收到 typed `blocked_by_player` 且不消费 moves、
  bookkeeping 或 current slot。下一 shared turn 的 first-player rotation 也必须轮换该冲突的首个合法 winner，避免固定
  host-first 优势。blocked player 必须显式选择另一 command 或 wait；服务器不得把 rejection 伪造为 accepted no-op。
- v1 move-bump 不隐式 swap 或攻击另一 human。未来若加入 swap/PvP，必须使用独立 typed command、稳定 target identity
  和明确的权限/死亡 policy，而不是复用 NPC menu、`swap_critters()` 或 legacy implicit melee。
- 同时拾取同一物品时，第一个合法命令成功，第二个收到 `state_changed`，并立即获得增量更新。
- 服务器日志记录 deterministic ordering key，便于复现。

### 8.3 菜单与“思考时间”

客户端打开背包、角色信息、地图或选择目标时，不应向服务器阻塞一个 C++ 调用栈。客户端可以停留在本地菜单；服务器把该玩家视为尚未提交动作，当前世界 barrier 暂停。

可配置策略：

- `wait_forever`：私服默认，无超时。
- `auto_wait_after_seconds`：超时后自动执行 wait。
- `pause_when_disconnected`：有人短暂断线时暂停整个 barrier。
- `host_controls_pause`：由房主手工暂停/继续。

### 8.4 长活动与快进

- 制作、阅读、睡眠、建造等 activity 继续按服务器秒推进。
- 只有所有在线玩家都处于“允许快进”的 activity，且任何玩家附近都无危险、无待处理 prompt 时，服务器才可批量推进多个 turn。
- 任一玩家发现敌人、受到伤害、activity 中断或发出普通动作时，立即退出快进。
- 快进批次仍需产生检查点和中断检查，不能直接跳过整个 activity 时间。

### 8.5 断线策略

推荐默认：

- 0 至 30 秒：保留玩家在 barrier 中，等待 session resume。
- 超过宽限期：只在该玩家成为 current disconnected slot 时应用 policy；先执行一次权威 wait，成功后再将当前
  snapshot 记录标为 finished 或 removed，由权威 session/roster owner 决定是否进入后续 turn。角色留在世界中并
  仍可受伤或死亡。
- 断线顺序必须是：transport 标记 disconnected -> scheduler 标记该 barrier participant disconnected -> 在
  registry 仍持有且可激活的 runtime 上执行 forced wait -> 记录 barrier terminal state -> session directory 再决定
  runtime 是否进入 offline。不得先调用 runtime disconnect/offline 再尝试自动 wait。
- `game::disconnect_multiplayer_player()` 仍不负责把 selected root 直接置为 offline。ADR-0010 的
  `multiplayer_single_root_owner` 已把 selected context 与 runtime online/offline lifecycle 解耦：在当前 barrier、
  actual world 和 player-end/lifecycle boundary 完成后 exact offline 并进入 dormant；dormant 禁止 guard/turn/command，
  resume 在同一稳定 runtime 上 exact reactivation 后才解除 dormant。不得绕过 owner、在 offline root 上继续跑 turn
  或引入临时 dummy player。
- 安全区服务器可以配置“安全下线后移除实体”，但必须有明确的安全判定和冷却，不能成为战斗逃生手段。
- 重连必须携带 session resume token、客户端最后接受的 generation 和最后确认 revision/sequence；服务器用 token
  record/runtime 交叉验证。session directory 对新 resume 只允许统一 generation 的精确 `+1`，对上一 accepted
  response 丢失只允许同 fingerprint 的同代幂等 completion replay；服务器决定发送 delta 还是 full snapshot。

## 9. 主循环拆分计划

把 `game::do_turn()` 逐步拆成以下阶段，单人模式也通过同一 scheduler 驱动一个玩家：

```text
begin_world_turn
  increment calendar / per-turn global work
  weather and timed-event preparation

begin_player_turn(player)
  theft/mount checks
  update_body
  activity pre-processing
  visibility/sound preparation

run_player_commands
  round-robin commands
  automatic activity steps
  death/disconnect handling

process_bubble_turn
  falling / vehicles / fields / items / explosions
  monsters / NPCs / sounds / scent

end_player_turn(player)
  process_turn and replenish moves
  body temperature/wetness/morale/power
  per-player messages and interruption state

end_world_turn
  publish revisions
  autosave decision
```

现有 `multiplayer_turn_phase_trace` 的 `turn_begin`、`player_begin`、`player_input`、`world`、`player_end` 只是
profiling/test 观测点，不是上述目标函数的现成所有权边界。Phase 3 source audit 已确认：

- `player_begin` 混合 overmap hordes、weather/NPC/light cache 等 world-once 工作与 body/activity/sound marker 等
  player-scoped 工作；
- `player_input` 每次轮询还包含 falling、dead cleanup、explosion 和 sound/visibility 副作用；
- `world` 只为当前 `u` 写 scent/field，并让 single active avatar 影响 monster/NPC/group-center 语义；
- `player_end` 同时包含当前 `u.process_turn()` moves replenishment、逐玩家规则和本地 UI/SFX。

当前 source 的普通单人 `do_turn()` 继续保持 legacy 顺序；production remote path 使用显式
`do_turn_remote_owned()` hooks。adapter 在 active-player guard 内执行 semantic action 和唯一一次 bookkeeping；不可
跳过的 player-phase-complete hook 处理 sleep/activity/zero-moves 分支；`multiplayer_owned_world_thunk` 令 actual
`process_legacy_single_player_bubble_turn()` 只能被 exact world claim 调用一次；player-end 全部返回后 owner 才记录
lifecycle completion。回归测试覆盖 action/world exact-once、zero-action terminal transition、伪造或重放 completion
proof 的 fail-stop，以及现有 moves replenishment 顺序；`check_safe_mode_allowed()` 读取当前 active avatar。

这些边界关闭了 single-root compatibility route，但 trace label 仍只是观察点。Gate 3 source
`2eccb92087991966423c18b63f6ef707462b1428` 已把已有 two-runtime wait-only contract 提升到独立 inner
multi-runtime owner；source `3204f8f45606a20ea6ab0892369b9806f89c4353` 又增加 thin command router、verified adjacent empty-ground move 和
move/move/wait/wait 隔离；source `84d8ca056bf72ed9776890f7ca4212a3bfdf7c2e` 再把 adjacent semantic move 的 human occupant 在 legacy
movement 前裁决为 typed block，并验证公平首位也轮换 contested-tile winner。plain-walk 直接链已改为读取 active
avatar，但这不代表完整 movement tree 或全局 collision invariant 已适配：绕过 router 的 forced movement、
monster/NPC attack、door/furniture/vehicle/grab/phasing/swim、field/trap/effect 等分支仍须逐项验证或拒绝。不能按 trace
label 机械切块，也不能把 inner owner/router 直接当作 production session owner。当前下一规则 slice 是 monster
target/attack 与 death/game-over safe boundary。

迁移时要逐项处理当前只对 `u` 执行的逻辑，至少包括：

- `update_body()`
- activity drain
- sound markers
- `creature_in_field()`
- scent source 写入
- `process_turn()`
- body temperature、wetness、morale
- power balance
- death/game-over
- autosave 与消息

服务器构建必须跳过 `play_music()`、`sfx::*`、`ui_manager::redraw()`、截图和窗口刷新；对应信息通过网络事件交给客户端。

## 10. 动作与 UI 解耦

### 10.1 拆分目标

将当前 `game::handle_action()` 拆为三层：

```text
local input resolver
  physical input -> action identifier

command builder / local action router
  local-only action -> local UI
  world-changing action -> typed network command

server command executor
  validate -> execute existing game rule -> result/events
```

现有 `do_regular_action()` 不应直接删除。迁移一个动作时，把该 case 的规则提取为可接受显式参数的 executor，然后：

- 单人模式：本地 command builder 调用同一个 executor。
- 联机客户端：本地 command builder 发送 command。
- 服务器：验证后调用同一个 executor。

move command 的 destination conflict 也属于 server rule result，不是客户端输入解析。v1 中 bump 到另一 human 只产生
authoritative `blocked_by_player`；swap 和 PvP attack 若以后启用，必须是独立 command/policy，不能由同一个 move payload
根据客户端猜测或 legacy UI 分支隐式决定。

### 10.2 动作分组与顺序

| 组 | 例子 | 实现策略 | 优先级 |
| --- | --- | --- | ---: |
| 纯本地 | zoom、fullscreen、panel、keybindings、help、tileset reload | 客户端直接执行，不发网络 | 0 |
| 简单无选择 | move、pause、move mode、autoattack | 直接 typed command | 1 |
| 相邻目标 | open、close、smash、grab、peek、interact | command 带绝对目标坐标与 base revision | 1 |
| 简单物品 | pickup、drop、wear、takeoff、wield、use、eat、read、reload | 客户端菜单选 `item_uid`，服务器复核 | 2 |
| 目标战斗 | fire、throw、spell、reach attack | 两阶段 query/commit，服务器给合法目标模型 | 2 |
| 长活动 | craft、construct、disassemble、butcher、sleep | 选择 recipe/target 后创建服务器 activity | 2 |
| 复杂界面 | advanced inventory、zones、vehicle UI、NPC trade、computer | 为现有 UI 提供远程 data provider | 3 |
| 管理动作 | save、quickload、debug、world mods | 客户端禁用或映射到服务器管理员命令 | 4 |

`move` 的 human-occupied destination 在“简单无选择” executor 前由 authority resolver 处理；它不会打开 interact menu、
调用 `swap_critters()` 或自动执行 melee。显式 swap/PvP 的稳定 target、双方状态、moves cost、权限、伤害与死亡结果属于
独立后续动作覆盖。

### 10.3 禁止服务器阻塞 UI

最终服务器 command executor 内不允许调用：

- `query_yn()`
- `uilist::query()`
- `string_input_popup`
- inventory selector 的阻塞输入
- 任何 SDL/curses/ImGui API

共享 rule seam 必须显式传播 `allow_interactive_ui = false` 或等价的 server execution mode。若 legacy branch 会进入
危险地形确认、NPC menu、物品选择或其他 popup，router 必须在 gameplay side effect 前拒绝，不能依赖 headless
suppression 假装该动作可执行。Gate 3 的 verified adjacent empty-ground move 已按此约束把 non-interactive mode 传入
plain-walk seam；typed human collision resolver 还会在 `move_effects()`、NPC menu、monster attack、`walk_move()` 和
`place_player()` 之前返回。这不自动批准其他 movement branch 或绕过 router 的 forced movement。

复杂交互改为：

1. 客户端发送 `QueryAvailableChoices`。
2. 服务器基于当前 revision 返回只读 choice model。
3. 客户端用现有本地 UI 展示。
4. 客户端发送包含稳定 ID 的 commit command。
5. 服务器再次验证状态、距离、可见性、数量和 moves。

不能通过“服务器线程等客户端按键”来长期兼容菜单，因为一个玩家停在菜单会冻结所有连接，也无法安全支持多个同时 UI。

### 10.4 稳定引用

网络命令只允许使用：

- `player_id`
- 服务器 `character_id`
- `item_uid`
- recipe/spell/terrain/furniture 等字符串 ID
- absolute map coordinate
- `entity_ref { kind, stable_id, revision }`

禁止发送：

- 内存地址
- vector/list 下标作为长期身份
- `action_id` 的整数枚举值
- 客户端计算的伤害、命中、移动成本或可见性结果

inner collision result 可以使用 exact participant key 验证 blocker owner，但该 key 的 session generation 不是可直接
公开的 scene identity。outer command-result/scene owner 必须先按目标玩家 visibility projection 决定是否只返回 generic
occupied result，或映射为可见的稳定 `entity_ref`；不得发送 raw pointer、registry index 或不可见玩家的连接代次。

动作标识使用稳定字符串或 FlatBuffers enum，并单独维护 protocol version。

## 11. 客户端状态与渲染

### 11.1 不同步完整地图

客户端不加载服务器 save 目录，也不接收完整 reality bubble 的内部 `submap` 对象。完整地图同步会：

- 泄露未探索地形、陷阱、怪物和隐藏物品。
- 把协议绑定到频繁变化的内部存档格式。
- 迫使客户端运行大量不必要的模拟。
- 增加作弊面和移动端内存占用。

### 11.2 `remote_scene` 模型

服务器基于该玩家的 FOV、map memory 和特殊视觉生成可见场景：

```text
SceneSnapshot
  revision
  center_abs
  z_level
  viewport bounds
  ordered sprite commands
  ASCII fallback cells
  overlays/cursors/target lines
  visible creature summaries
  weather/lighting state
  sound events
```

每个 sprite command 至少包含：

- 相对/绝对 tile 坐标
- tile ID
- tile category 与 subcategory
- subtile 与 rotation
- lit level / vision effect
- layer order
- 可选颜色、透明度和动画参数

客户端新增 `cata_tiles::draw_remote_scene()`，继续调用本地 `draw_from_id_string()`，因此客户端可自由使用自己的 tileset、缩放和窗口尺寸。

服务端 scene builder 应从地图和角色可见性生成语义层，而不是依赖 SDL。长期目标是本地单人 tile renderer 也能消费同一种 scene DTO，以测试本地与远程画面一致性；首个垂直切片可以先覆盖常见 terrain/furniture/item/vehicle/creature 层。

### 11.3 玩家自身镜像

客户端可保留一个只读 avatar replica，用于复用：

- 角色面板
- 角色信息
- 背包与穿戴列表
- 技能、状态、bionics、mutations
- 本地菜单排序和筛选

首版可以在关键动作后发送压缩的完整“自己的 avatar snapshot”，随后再优化为字段级 delta。客户端 replica 的任何修改都不能直接提交为结果，只能生成 typed command。

### 11.4 消息、语言和音效

- 世界内容尽量通过稳定 ID 和数值传输，在客户端本地翻译。
- 暂时无法结构化的游戏消息可先发送服务器格式化 UTF-8 文本，但应标记为兼容债务。
- `Messages` 必须按玩家隔离；聊天单独作为结构化事件。
- 服务端发送语义 sound event：sound ID、variant、位置、音量和类别。
- 客户端根据本地 soundpack 播放，服务器不初始化 SDL mixer。
- 天气动画由客户端根据服务器天气状态本地生成，不传视频帧。

### 11.5 不做客户端预测

CDDA 是回合制，首版不需要 movement prediction。客户端可以显示“命令已发送”状态，但在服务器确认前不改变 canonical 位置和库存。这样可避免回滚复杂度及移动、开门、车辆、陷阱等状态不一致。

## 12. 网络与协议

### 12.1 传输选择

推荐首个实现：standalone Asio + TCP。

理由：

- CDDA 命令量低、要求可靠有序，不需要为实时动作引入 UDP 重传和拥塞控制。
- Asio 支持 Linux、Windows 和 Android，且可以保持网络层独立于 SDL。
- TCP 分片和粘包由明确的 frame codec 处理。

安全模式：

- 开发和 LAN 模式可显式允许 plaintext。
- 默认只绑定 loopback，除非配置了 TLS 或管理员明确启用不安全监听。
- WAN 发布必须使用 TLS 1.3，或明确要求通过 WireGuard/Tailscale 等外部加密隧道。
- 不自行设计加密算法。Phase 0 没有选择或打包嵌入式 TLS，因此首个实现明确采用
  `loopback / 显式受信 LAN / 外部认证加密隧道` 限制；不能默认明文监听公网。

Phase 0 验证使用 standalone Asio `1.38.1`，固定 tag `asio-1-38-1`、commit
`dfd7b3e3145bac5d0e91a99fde69c6ae1442f971`、archive SHA-256
`2827b229972be80cdb14e5497962fa393d1adf036b5869e2b9c99f644daadacc` 和 Boost Software
License 1.0。Linux GCC/Clang、Windows MSVC 和 Android NDK arm64 编译门禁已经通过。生产依赖仍须按
仓库第三方依赖方式接入，且不能链接或扩展 `tools/multiplayer/transport_spike/` 的测试程序。网络接口应
抽象为 transport，使未来 TLS 后端不影响协议和模拟代码。

### 12.2 协议编码

推荐使用 FlatBuffers schema：

- 项目已经内置 FlatBuffers runtime。
- `.fbs` schema 版本化，生成头文件提交到仓库。
- CI 使用固定 `flatc` 版本验证 generated header 未过期。
- 所有入站 buffer 先做 verifier 和业务级限制检查。
- 大 snapshot 使用现有 zstd，普通小 command 不压缩。

不要直接把 save JSON 当成通用网络协议。Portable character 可以在 envelope 内承载独立、严格白名单的 JSON/FlatBuffer，但不能调用 `game::unserialize()` 加载整个不可信 save。

### 12.3 Frame 与限制

每个 frame 至少包含：

- magic/version
- protocol major/minor
- message type
- flags
- session ID
- request/client sequence
- payload length
- uncompressed length
- checksum/MAC 由 TLS 或 frame 层负责

必须限制：

- 最大 frame 大小
- 最大解压后大小
- 最大压缩比
- 每连接命令速率
- 未认证连接握手时间
- pending request 数量
- snapshot chunk 数和重组内存
- 字符串、聊天、人物包、物品树深度与数量

### 12.4 核心消息

握手和连接：

- `ClientHello`
- `ServerHello`
- `AuthChallenge`
- `AuthResponse`
- `ContentManifest`
- `JoinRequest`
- `JoinAccepted` / `JoinRejected`
- `ResumeRequest`（携带客户端最后接受的 session generation）

游戏：

- `PlayerCommand`
- `CommandResult`
- `FullSnapshot`
- `StateDelta`
- `SceneSnapshot` / `SceneDelta`
- `ChoiceModel`
- `ChatMessage`
- `ServerNotice`
- `Ping` / `Pong`
- `DisconnectNotice`（客户端请求与服务端确认使用同一 typed message，sequence 必须精确匹配）

### 12.5 顺序、幂等与 revision

- 每个客户端 command 带递增 `client_seq`。
- 服务器缓存最近 N 个 command result，重发同一 seq 时返回相同结果，不重复执行。
- server command-result cache 与 resume 可重放 `PlayerCommand` sequence 使用同一个有界 N；application
  high-water 单调记录已见业务序列。只有仍保留且原类型为 `PlayerCommand` 的 sequence 可以重放；ping、
  sequence gap、解析/入队失败和已淘汰 command 的 sequence 不得在重连后跨类型复用，过旧 replay floor 必须
  令 session 过期而不是把旧 sequence 当新命令执行。
- command 带 `base_revision`；依赖旧场景的动作在 revision 不匹配时重新验证或拒绝。
- delta 带 `base_revision` 与 `new_revision`；客户端缺失中间 delta 时请求 full snapshot。
- TCP 有序不等于业务幂等，重连时仍必须处理“服务器执行成功但客户端未收到确认”的情况。
- resume admission 本身也必须幂等：客户端在 accepted resume response 丢失后会以同一旧 generation/revision/
  sequence 重试，服务器应重放已提交的 next generation，而不是再次递增；客户端已接受 response 后的下一次 resume
  则携带新 generation 并推进一代。
- clean `DisconnectNotice` 必须在 simulation FIFO 中晚于既有 command/result/scene；服务端只有在 ACK bytes
  写完且 transport 报告精确 ordered-close completion 后才能删除 resume record，任何 queue/read/write/peer-close
  failure 都保留 resume 能力。

## 13. 版本与内容兼容

握手必须比较：

- protocol major/minor
- server state schema version
- savegame version
- multiplayer build ID
- active mod ID 及顺序
- core、mod、world custom mod 的内容 manifest/hash
- 必需 feature flags

建议规则：

- protocol major 不同：拒绝。
- minor 不同：仅在 capability negotiation 明确兼容时允许。
- gameplay data hash 不同：拒绝。
- tileset、字体、语言、soundpack 不参与 gameplay hash。
- 客户端缺少服务器 mod 时给出缺失列表，不自动执行下载或安装。

protocol minor `1` 引入 `ResumeRequest.session_generation`，属于不兼容的 session-admission 语义边界；当前严格握手
应拒绝 minor `0`，不能把缺失 generation 当作 `0` 或仅依赖 token 猜测。该 Tier 2 public boundary 必须验证
generated header、Linux production protocol/client/server，以及实际编译 changed production source 的 MSVC/NDK
边界；它不同时宣称 phase exit、release 或跨版本互通，因此不单独触发 Tier 3 产品矩阵。

multiplayer build ID 是后端无关的规范源码身份，格式必须是 40 位小写 Git SHA，可选追加 `-dirty`，即
`^[0-9a-f]{40}(-dirty)?$`。Make、CMake、Gradle 和 MSVC 必须为同一 source tree 生成同一个值；显示用
`VERSION`、`+SDL3`、tiles/sound capability、generator tag 或包名不能参与握手身份。构建系统和 CI 应显式注入或
从完整 Git HEAD 推导并验证该值，`--version` 和 artifact provenance 必须暴露它；无法取得合法身份时 multiplayer
server/client 启动应 fail closed，同一源码不同 UI/backend 应能握手，不同 SHA 或 `-dirty` 状态应被拒绝。生成器
不得因已有 `version.h` 而跳过重新核对 source identity，显式 override 必须贯穿实际 build target；Git dirty-state
probe 的基础设施错误不能降级成 `-dirty`，CI 对 canonical ID 的文本断言必须区分大小写。

内容 hash 应对规范化相对路径与文件内容做 SHA-256，并缓存结果。manifest 需要包含 world-local `mods`。只比较 mod 名称或版本字符串不够，因为本地修改后的 JSON 仍可能同名。

## 14. 客户端人物与 Portable Character

### 14.1 为什么不能直接上传 `.sav`

当前 `game::serialize_json()` 同时包含：

- turn/calendar
- 当前 map 坐标
- scent
- active monsters
- kill/stats/achievements
- player
- global EOC queue
- messages 和 unique NPC 状态

完整 `.sav` 不是纯人物文件。直接导入会覆盖服务器世界时间、怪物、RNG 关联状态和地图位置，并可引入重复 character/item ID。

### 14.2 独立人物包

新增版本化 envelope，例如：

```text
PortableCharacterPackage
  package_version
  source_build/content_manifest
  character_generation
  lease metadata
  avatar payload
  optional cosmetic metadata
  signature
```

服务器只读取独立 avatar payload，并通过专用 importer 执行白名单迁移。

### 14.3 默认保留与清理规则

通常可保留：

- 姓名、外观、性别、身高
- stats、skills、traits、mutations
- HP、需求、疾病和生理状态
- bionics、spells、proficiencies
- inventory、worn、wielded items
- learned recipes、identified items、snippets
- 角色历史中不引用原世界对象的部分

必须重置或重映射：

- `character_id`：服务器重新分配。
- 所有 `item_uid`：递归重新生成。现有 `item_uid` copy 语义会生成新 UID，可用于安全复制，但需重建 item references。
- 坐标、dimension、world origin、start location：由服务器决定。
- active/completed/failed mission 引用：默认清空；只允许服务器重新授予。
- follower、faction representative、camp、zone、NPC 引用：清空。
- remote vehicle、grab、mounted、translocator 等世界引用：清空或严格验证。
- current activity、destination、auto move：取消。
- map memory、overmap seen/notes：不从别的世界带入。
- queued EOC 和指向旧世界 item/location 的 safe references：清理。
- item owner/faction：按服务器人物 faction 重设。
- debug 状态和不允许的物品/traits：按服务器 policy 校验。

### 14.4 三种人物策略

| 策略 | 行为 | 适用场景 |
| --- | --- | --- |
| `server_owned` | 人物只存在服务器，客户端仅选择账号人物 | 公服或最严格模式 |
| `portable_lease` | 客户端人物被“签出”给服务器；服务器持有 canonical generation，正常退出后签名回写 | 推荐私服默认 |
| `copy_in` | 上传一份副本，之后服务器与本地各自发展，不自动合并 | 完全信任的休闲私服 |

推荐实现 `portable_lease`：

- 人物包有 generation 和 lease ID。
- 客户端本地标记 checked out，不能在另一世界继续同一 generation。
- 服务器只接受最新 generation。
- 正常退出时导出下一 generation 并签名。
- 异常断线时 canonical copy 仍在服务器，下次重连继续，不生成分叉。
- 管理员可以为私服开启 unsigned import，但 UI 必须明确提示可复制物品和角色。

不存在安全且自动的“同一个人物同时在单人世界和服务器世界发展后再合并”方案。技能、物品、伤势、任务和唯一物品会发生不可判定冲突，v1 不做双向 merge。

## 15. 世界创建与 Headless Server

### 15.1 运行模式

引入：

```cpp
enum class runtime_mode {
    local_client,
    network_client,
    dedicated_server,
    test
};
```

CLI 示例（当前已实现的 server 操作）：

```text
cataclysm --init-server-config server.json
cataclysm --check-server-config server.json
cataclysm --server server.json
```

图形/终端客户端现已在 Phase 2 实现 `cataclysm --connect example.org:27999` /
`cataclysm-tiles --connect example.org:27999`，并通过
`--connect-token-file` 从私有文件读取 bearer token；非 loopback plaintext 仍需显式 LAN 例外。
首个阶段由现有 curses/tiles binary 增加 `--server` 并跳过界面初始化，即使仍链接 UI backend。之后再增加独立
`cataclysm-server` target，去除运行时窗口依赖。不要把“完全不链接 curses”作为第一个联机切片的阻塞项。

### 15.2 服务器配置

当前 schema 1 的生成配置采用“只声明已交付能力”的严格默认值（省略了认证和限流字段的完整示例）：

```json
{
  "schema_version": 1,
  "world": {
    "name": "coop-world",
    "seed": "server-owned-seed",
    "mods": [ "dda" ],
    "options": {},
    "spawn_policy": "shared_start"
  },
  "network": {
    "listen": "127.0.0.1:27999",
    "security": "disabled",
    "allow_insecure_lan": false
  },
  "players": {
    "max": 1,
    "character_policy": "server_owned",
    "tether_tiles": 48,
    "disconnect_grace_seconds": 30
  },
  "time": {
    "policy": "turn_barrier",
    "idle_timeout_seconds": 0,
    "fast_forward": "all_players_safe"
  },
  "save": {
    "interval_turns": 300,
    "keep_generations": 1
  }
}
```

Phase 3 的纯 scheduler policy 切片不改变配置能力。只有它接入 production session directory、forced/scoped
auto-wait、真实 phase/world callback，并关闭两玩家 collision、monster/death、field/scent/NPC、tether/group-shift
和 messages/safe-mode/stats/player-scoped cache isolation 门禁后，parser 才能接受 `players.max > 1`；在此之前必须
继续拒绝。Phase 5 的 generation
save 和人物导入完成前，必须拒绝 `keep_generations > 1`、`portable_lease` 和 `copy_in`，不能让配置伪装为已生效。
对应阶段完成后再扩大合法范围，最终私服推荐值仍是 2 至 4 名玩家、多个 save generations 和
`portable_lease`。

这是未内嵌 TLS 时的安全默认值。非 loopback 地址只有在管理员显式设置受信 LAN 例外，或将
`security` 设为 `external_tunnel` 后才可接受。嵌入式 `required` 只有在后续 TLS 三平台门禁完成后才允许；
当前 parser 必须拒绝它，而不是静默降级。证书和私钥字段也不得在没有对应 backend 时伪装为生效。

配置加载必须：

- 使用结构化 parser。
- 拒绝未知关键安全字段或至少记录明确警告。
- 校验世界名和所有路径，禁止客户端参与路径拼接。
- 在启动前验证 mod 冲突和 content manifest。
- 默认不覆盖已有世界。
- 支持 `--check-config` 和 `--init-server-config`。

### 15.3 世界 bootstrap

当前 `game::start_game()` 同时创建世界环境和初始化唯一新角色。需要提取：

- `bootstrap_world()`：calendar、weather、master state、共享出生 OMT、地图初始生成。
- `spawn_new_player()`：在已有世界放置新人物。
- `import_player()`：清理 portable character 后放置。

不能为每名加入者重复执行 `start_game()`，否则会重复发 profession items、重置世界状态或再次生成起点。

### 15.4 Headless 禁止项

服务器启动路径不得调用：

- `catacurses::init_interface()`
- `game_ui::init_ui()`
- `cataimgui` 初始化
- SDL tiles/sound 初始化
- language selection popup
- main menu

`debugmsg` 在服务器中必须写日志并触发可控错误策略，不能尝试打开 popup。任何 command executor 触碰 UI 都应在测试构建中断言失败。

### 15.5 Android 与 Windows

Android：

- `AndroidManifest.xml` 增加 `android.permission.INTERNET`。
- 网络连接必须适应 app pause/resume、屏幕旋转、后台切换和系统杀进程。
- session token 存在 app private storage，不放公共 external storage。
- 网络读写不得阻塞 SDL 主线程。
- 保存最近服务器、证书指纹和人物 lease 状态。

Windows：

- GUI 主线程保持 SDL/ImGui；Asio 在独立 I/O 线程。
- 支持 DNS、IPv4、IPv6。
- 错误提示转换为本地 UI，不把 socket error 直接暴露为不可读异常。

## 16. 存档重构

### 16.1 拆分现有混合 player save

新增：

- `serialize_world_runtime()` / `deserialize_world_runtime()`
- `serialize_player_runtime(player_id)` / `deserialize_player_runtime(player_id)`
- `save_server_world()`
- `save_server_player(player_id)`

现有单人 `serialize_json()` 继续组合 world runtime + 单一 player runtime，维持旧 save 兼容。联机服务器只写一次 world runtime，并为每名玩家写独立 player runtime。

应从现有 player save 移入 world/bubble save 的字段：

- turn/calendar
- current bubble origin
- scent
- active monsters
- bubble/world EOC queues
- RNG engine state
- 其他真正共享状态

继续按玩家保存：

- avatar
- messages
- stats/achievements/memorial
- map memory
- player-specific automation rules
- session-safe activity state

### 16.2 RNG 状态

服务器权威仍需要在 save/restart 后精确恢复 RNG。当前代码设置 seed，但普通 save 没有显式保存 `std::minstd_rand0` 当前 engine state。新增 world runtime 字段序列化 RNG engine；恢复时在任何可能调用 RNG 的加载迁移前设置。

### 16.3 保存一致性

第一阶段：

- 只在 barrier 静止点保存。
- 暂停接受世界命令，聊天仍可排队。
- 每个文件继续使用 temp + atomic rename。
- 保存前创建可恢复备份。
- 写 `save_in_progress` marker；启动时检测并回退。

生产阶段：

- 使用 generation 目录。
- 通过 reflink/hardlink/copy-on-write 保存未变地图文件，必要时 copy fallback。
- 所有新 generation 文件完成并校验后，最后原子更新 `CURRENT`。
- 保留最近 N 个 generation。
- 启动时验证 manifest、hash 和必需文件，失败则回退上一代。

建议逻辑布局：

```text
save/<world>/server/
  CURRENT
  server.json
  generations/
    000001/
      manifest.json
      world_runtime.sav
      players/<player_uuid>/avatar.sav
      players/<player_uuid>/messages.sav
      map snapshot/reference set
```

### 16.4 自动保存

- 由服务器统一计划，不使用每玩家 `AUTOSAVE`。
- 支持 turn 间隔和真实时间上限二者。
- 保存时间、文件数、字节数和失败原因进入结构化日志。
- 保存失败不得继续覆盖上一代有效 save。

## 17. 单 Reality Bubble 首版

### 17.1 为什么首版需要距离约束

当前 `map` 覆盖约 132 x 132 tiles，怪物、NPC、scent、sounds、visibility cache 都围绕同一现实泡。让玩家任意距离分离会立即要求：

- 多个 `map`
- 多个 creature tracker
- 多份 scent/sound/cache
- 重叠泡合并与拆分
- 防止同一 submap 被重复模拟
- 跨泡车辆、怪物和事件迁移

这会把首个版本的核心风险扩大一倍以上。

### 17.2 v1 规则

- bubble 中心根据所有在线玩家 bounding box 调整，而不是只跟随 `game::u`。
- 只在 turn boundary shift map。
- shift 时同步所有 human players、monsters、NPCs、scent 和各类 bubble-relative cache。
- 玩家间最大 x/y span 必须小于 map 安全范围并留出视距边缘。
- 即将越界的移动由服务器拒绝，并返回可本地化原因。
- 管理员可配置 tether，默认约 48 tiles，而不是使用完整 132 tiles 极限。

这仍允许玩家在同一建筑、街区或战斗区域内自由协作。

## 18. 多 Reality Bubble 后续阶段

只有单泡 v1 稳定后再实现：

```text
world_runtime
  bubble_manager
    bubble_runtime A: map + tracker + scent + sounds + players
    bubble_runtime B: map + tracker + scent + sounds + players
```

规则：

- 不重叠的玩家组可以拥有独立 bubble。
- 两个 bubble 覆盖范围接近或重叠时，在 turn boundary 合并。
- 玩家远离且不存在跨区实体引用时可拆分。
- 任意 submap 在一个 turn 内只属于一个 active simulation bubble。
- global calendar 仍只推进一次；所有 bubble 完成本 turn 后统一进入下一秒。
- overmap 粗粒度模拟继续是 world scope。
- `get_map()` 返回当前处理 map；`reality_bubble()` 拆成 active bubble accessor 与 all-bubbles API，落实 `map.h` 现有注释。

多泡前置重构：

- `game::m`、`critter_tracker`、`scent` 迁入 `bubble_runtime`。
- `swap_map` 扩展为 `simulation_context_guard`。
- sounds 静态容器迁入 bubble。
- 所有“距离唯一玩家”的 world/NPC 逻辑改为“最近 human player”或显式目标玩家。
- 重叠检测、实体所有权、bubble merge/split 有独立 invariant tests。

## 19. 安全设计

### 19.1 身份与认证

- 服务器创建一次性 invite code 或管理员批准流程。
- 客户端首次加入生成持久身份密钥/随机凭据并绑定 server account。
- 凭据只存私有目录，日志中脱敏。
- resume token 短期有效且可撤销。
- WAN 必须在 TLS 或外部加密隧道中运行。

### 19.2 命令验证

每个 command 都验证：

- session 与 player 是否匹配。
- 当前 turn/barrier 是否允许该玩家行动。
- base revision 是否可接受。
- 玩家是否存活、清醒、具有 moves、未被冲突 activity 锁定。
- 目标是否存在、可见、可达、在合法范围。
- item UID 是否仍由该玩家/位置持有。
- 数量、费用、技能、工具、弹药和权限。
- debug/admin action 权限。

绝不接收客户端提供的最终 HP、moves、库存、伤害或 RNG 结果。

### 19.3 输入与资源限制

- FlatBuffer verifier 后再做业务 schema 检查。
- 对字符串 UTF-8、长度和控制字符做限制。
- 人物包限制总大小、item 数、嵌套 pocket 深度和单 item 数据量。
- zstd 解压使用硬上限，防止压缩炸弹。
- 所有队列有容量和 backpressure。
- 超限连接先降速，再断开并记录原因。
- 禁止客户端指定文件路径、mod 路径、save 名或 shell command。
- 不自动加载客户端提供的动态库、脚本或 tileset。

### 19.4 管理命令

- 管理命令不复用普通 chat 文本解析。
- 使用独立 message type、权限和审计日志。
- 支持 `list/kick/ban/save/pause/resume/shutdown/teleport` 等最小命令。
- remote debug console 默认关闭，开发构建也不能对未认证用户开放。

## 20. 测试策略

### 20.1 单元测试

- frame codec 的分片、粘包、空包、超长包。
- FlatBuffer verifier 与所有长度限制。
- protocol version/capability negotiation。
- content manifest 与 mismatch 诊断。
- portable character 字段清理、ID 重映射、非法物品树。
- command validation。
- revision 与幂等缓存。
- turn scheduler 的 immutable roster snapshot、稳定首位轮换/roster churn、四玩家无饥饿、typed
  rejected/duplicate 不推进、generation resume、current-only timeout、`automatic_wait_pending` 和 world
  claim/record stale/double-call 拒绝。
- phase adapter 的 exact slot/player/generation、missing/stale/inactive runtime、registry ownership、root context、
  guard engagement/恢复、目标 action bookkeeping、真实 forced `pause()` before record、active-avatar safe-mode
  permission gate，以及
  callback/wait/world/record failure 后 scheduler 永久 fail-stop；替换 adapter 也不得重试已可能发生副作用的工作。
- multi-runtime command router 的 exact-current owner entry、共享 basic executor、post-action moves 到 scheduler
  disposition 的映射，以及 malformed/non-current/unsupported move 的 rejected-without-advance；任何 status/action
  不一致都必须 fail-stop，不能猜测一个 accepted disposition。human occupied destination 还必须返回 distinct typed
  `blocked_by_player` 与 exact blocker，重复 block 保持零 moves、零 bookkeeping、零 cursor advance 和 no-side-effect
  owner state。
- session directory/lobby 的 pending -> prepare/pre-encode -> simulation commit -> transport enqueue -> publish 顺序、
  重复/stale completion、同 token 并发 claim、exact generation、accepted resume response 丢失后的同代 replay、
  首个 application frame 的 exact-tuple directory confirmation 与显式 lobby ack、fresh/terminal token record 回收、
  ordered rejection queue fallback，以及旧 connection/session/generation tuple 拒绝。
- scene visibility filter，确保隐藏怪物、陷阱和未探索地形不出现在包中。
- save generation 与 fallback。

### 20.2 活动玩家上下文专项测试

在决定角色桥接方案前必须有：

- 两个地址稳定的 avatar 连续切换活动上下文 10,000 次后，对象地址、身份和状态 hash 不变。
- inventory、nested pockets、worn、wielded、item_location 在每次切换后均解析到正确 owner 和 UID。
- active activity 可在切出、切回后继续。
- guard 嵌套、提前返回和异常恢复保持严格 LIFO；非模拟线程使用触发断言。
- mounted、vehicle passenger、grab、remote control 完成上下文切换 round-trip。
- missions、map memory、diary、recipes、bionics、mutations 保持。
- `creature_at`、`shared_from` 和 `critter_by_id` 始终返回正确身份。
- ASan、UBSan、LSan 下无错误。
- Phase 0 player snapshot 保存/加载后，两个角色身份与玩家级状态一致，且不复制或修改共享
  world/map/vehicle 状态。完整 world restart、RNG 和 generation fallback 由 ADR-0007/Phase 5 验证。
- 两个 runtime 通过同一 command router 执行 `move -> move -> wait -> wait` 后，只有目标 avatar 的 position、moves、
  tracker index、`nv_cached` 和 action bookkeeping 改变；另一 avatar、root grab state、runtime owner 与 guard 外 root
  context 必须保持。当前只对 verified adjacent empty-ground subset 作正向声明。
- root/secondary 双向 human block 必须解析对方 exact participant key，并保持双方 position/moves/tracker/runtime/cache、
  global action counter、root context 与 fault/side-effect state 不变。两 turn contested empty-tile case 必须证明 first
  success/second block、无 avatar overlap，以及 next-turn first rotation 也轮换 winner。

完整 avatar move-swap 的引用失败测试作为对照保留。稳定地址方案若仍有核心不变量失败，先停止生产扩展并更新 ADR，不通过分散的 cache 修复掩盖问题。

### 20.3 集成测试

构建 in-process loopback harness：

- 一个 server、两个无 UI test clients。
- 当前 in-process source tests 已通过 existing registry/guard 让两个 runtime 完成 wait/forced-wait barrier，并经 thin
  router 执行 exact `move -> move -> wait -> wait`，验证目标 position/moves/bookkeeping 与 tracker/root-context 隔离；
  adjacent symmetric block 和连续两 turn contested-tile winner rotation 也已由 inner owner 覆盖。world 仍只对一个
  lambda 计数；它们不是两个 client，也没有执行真实 bubble gameplay callback、scene publish 或 wire result。
- production integration 必须把实际 legacy bubble/world callback 放在 `claim_world()` 后运行并验证恰好一次；只
  调用 policy 的 `record_automatic_wait_executed()`/`claim_world()` 或只计数测试 lambda 不能算最终 gameplay evidence。
- 两玩家相邻移动并互相阻挡：inner owner contract 已覆盖；production two-client/actual bubble 仍待完成。
- 同时拾取同一物品，只成功一次。
- A 攻击怪物、B 看到结果和声音。
- A/B 互相攻击、受伤、死亡和尸体处理。
- 制作/睡眠时另一玩家行动，barrier 正确。
- 开门、车辆、枪械、投掷、NPC 对话的多步命令。
- 断线发生在命令发送前、执行后未确认、snapshot 中途。
- server save/restart 后 resume。
- 老单人 save 加载、保存、再次加载。

### 20.4 网络故障注入

- 延迟、抖动、限带宽。
- 随机断开。
- frame 分片到单字节。
- 重复 command/reconnect replay。
- snapshot chunk 丢失和超时。
- 慢客户端导致发送队列积压。
- 恶意大人物包和高压缩比数据。

TCP 不会乱序交付同一连接中的字节，但业务测试仍要覆盖重连后的重复和旧 session 消息。

### 20.5 Fuzz

- frame decoder。
- FlatBuffer root/messages。
- portable character importer。
- zstd envelope。
- chat/UTF-8。
- server config。

所有 fuzz target 必须在无 UI、无真实网络、固定内存限制下运行。

### 20.6 分层验证与 CI 矩阵

验证同时按风险层级和交付频率组织，不要求每个 backend-neutral shared C++ 提交都重新完成 Windows package 和
Android APK。每次可交接的切片收口在 `STATUS.md` 记录所选层级、原因、精确命令、结果和按策略未运行的平台；
“本批按策略未运行”不是 blocker，也不能冒充对应平台证据。

本节是选择验证层级的唯一规范来源。默认从 Linux 开始，只有已经完成的切片或验收声明实际跨入平台专属、公共
兼容或发布边界时才升级。不得因为文件名含 `multiplayer`、代码是 shared C++，或某个 workflow 恰好有 Windows/
Android job，就自动要求完整全平台编译。反过来，任何 job 若没有编译或运行本次 changed production source，也
不能作为该变更的 Windows/Android 证据；isolated transport spike 只证明 spike、Asio pin 和对应 compiler/toolchain
contract。

验证频率分为三种：

- **编辑循环**：开发中的中间提交或本地修改只运行能快速暴露当前问题的 Linux compile/focused tests。除非正在
  诊断平台专属失败，不为同一未收口切片的每个提交重复 Windows package、Android APK 或设备测试。
- **切片收口**：一组可交接的 scheduler、rule、session、protocol、client 或 server 行为完成后，以当前 Linux
  候选 source 运行 native client + headless server 默认闭环，并按风险增加完整 `[multiplayer]`、sanitizer 和真实
  PTY/loopback。`STATUS.md` 的日常证据以这个频率记录，而不是把每条编辑命令写成独立平台门禁。
- **平台里程碑**：Windows/Android owned code、公共 wire/ABI/toolchain 边界或平台产品声明形成一个完整关键批次后，
  对冻结的该批候选补受影响平台证据。同一关键批次的中间提交不逐个重跑平台；平台门禁之后若又修改该边界，才
  需要对新的候选重跑。

**Tier 1：Linux 默认闭环。** 这是内部 scheduler、game rule、session/protocol 状态机、服务器和 portable
transport 变更的默认层级，前提是没有修改 wire schema/version、public header 或平台 adapter：

- 编辑循环在 Linux GCC 上构建受影响的 native client/server 或 tests，并运行 changed-area focused tests。
- 切片收口时，Linux headless `--server` 与 Linux native client（curses 或 SDL）是 backend-neutral client/server
  功能的默认验收组合；接通 production transport、session、command 或 UI 路径时运行真实 PTY/loopback smoke。
- 共享 turn、authority、session、protocol、visibility、save 或 transport invariant 改变时，增加完整
  `[multiplayer]` suite；涉及地址、生命周期、线程或内存所有权时按风险增加 ASan/UBSan/LSan。
- 触及 SDL/tiles renderer 时使用 Linux graphical client；否则 curses client 足以验证共享 semantic executor 和
  protocol。Linux 结果不证明 Windows package、Android Activity lifecycle 或其他平台专属行为。

**Tier 2：平台专属或跨平台公共边界定向验证。** 只有完成的关键批次跨入以下边界时才要求对应平台；门禁按切片
收口或平台里程碑运行，而不是按每个提交运行：

- Windows：`msvc-full-features/`、batch/PowerShell/windist、MSVC-only compiler boundary、Win32 filesystem/process/
  socket/ACL 或 Windows SDL input/rendering。按验收对象运行 hosted MSVC compile、loopback、package 或 native smoke；
  只有 package/runtime 是目标时才要求完整 tiles+sound package。
- Android：Gradle/CMake/manifest、Java/JNI、ABI/resources、SDL touch/rendering、credential storage、Activity
  pause/resume/reconnect。按验收对象运行 NDK compile、目标 ABI APK/resource smoke；触及 lifecycle/runtime 行为时
  必须再跑 emulator 或 device，compile-only APK 不能替代运行证据。
- 跨平台公共边界：wire schema/version/capability/build ID、公开 DTO/serialization layout、compiler/ABI-sensitive
  public header、共享 source-list 或 pinned toolchain contract。Linux 主门禁仍必须运行，再选择能实际编译 changed
  production source 的 MSVC/NDK 最小 gate；package/resource/runtime 不是验收目标时，不升级为完整包。
- backend-neutral `src/multiplayer_*` 内部 `.cpp` policy/rule/phase-adapter 实现、测试或不改变接口/协议的重构仍属
  Tier 1；若新增或修改 platform conditional、compiler-sensitive ABI/header 或平台 adapter，只补受影响平台。
- MinGW/NDK 等廉价 cross-compile smoke 只证明该编译边界；它不能关闭 MSVC package、Android APK resource 或
  emulator/device lifecycle gate。反过来，非当前验收目标的平台完整 package 也不是 shared-code 切片收口前置。

**Tier 3：阶段出口、发布与兼容性里程碑。** Tier 3 是一次候选证据审计，不等同于自动运行全平台：

- 每个 phase exit 必须在当前候选 source 上完成 Linux GCC headless server、unit/integration 和 native client process
  smoke，并按该阶段风险运行 Linux sanitizer gate；这是不可由历史结果替代的当前候选主门禁。
- Windows/Android 只有在本阶段自其最近兼容证据后修改了对应 owned code、公共 wire/ABI/toolchain 边界，或阶段出口
  新增/改变该平台产品声明时，才必须为当前候选补新的定向 compile/package/runtime 证据。
- 若阶段没有触及上述边界，可以引用最近兼容平台证据，但必须记录 evidence commit/run，并对
  `git diff --name-status <evidence_commit>..<candidate_commit>` 及相关路径/条件分支做 diff audit，确认没有相关
  platform-owned、public boundary、source-list、toolchain 或 platform conditional 变化。该记录只能表述为“沿用兼容
  证据”，不得写成当前候选已在该平台编译或运行；diff 无法可靠审计时必须重跑受影响平台。
- release candidate、正式发布、明确的跨版本互通承诺或跨平台产品兼容声明，仍须在同一候选 source 上运行与声明
  匹配的必要平台矩阵。pinned compiler/NDK/JDK/vcpkg/Asio/FlatBuffers、workflow、artifact/ABI/signing contract 的
  里程碑变更必须覆盖受影响的平台。
- 单次 append-only schema/public DTO 变更或 protocol minor bump 若不同时宣称 phase exit、release 或跨版本产品兼容，
  按 Tier 2 公共边界定向验证；本次 minor `1` 属于该情况，不因改了 version 字段自动升级为全平台 Tier 3。
- Android emulator/device、单人 save compatibility、跨平台 loopback、fuzz/soak 只在对应阶段退出标准或发布风险要求
  时纳入。每夜 4-client soak 可独立运行，不把其每次结果变成日常提交的同步前置。

| 频率/变更类别 | 默认验证 |
| --- | --- |
| 编辑循环；内部 shared rule/scheduler/adapter 实现 | Linux compile + focused tests |
| 切片收口；backend-neutral client/server 行为 | 当前 Linux native client + headless server；按风险增加 full `[multiplayer]`/sanitizer/PTY |
| 完成的公共 header/schema/ABI/toolchain 关键批次 | Linux 主门禁 + 实际编译 changed source 的 MSVC/NDK 最小定向 gate |
| 完成的 Windows 或 Android owned build/runtime/UI/lifecycle 批次 | Linux 回归（若影响 shared code）+ 受影响平台的 compile/package/runtime gate |
| phase exit | 当前 Linux 候选必跑；平台边界有变化则补新证据，否则记录最近兼容 evidence commit + diff audit |
| release、artifact/signing 或显式跨版本/跨平台产品里程碑 | 与声明匹配的同候选必要平台矩阵 |

CI automation 与该策略对应：日常 Tier 1 使用 production transport workflow 的 `target=linux`，不调用 package
baseline。baseline workflow 的手工 dispatch 技术默认仍是 `target=all`，但只用于显式 package/release 矩阵；任何
手工 baseline 都必须按已完成批次的验收目标显式确认 Linux、Windows、Android 或 `all`。自动 push/PR 先按
changed path 选择 package target：Windows-owned path 只跑 Windows，Android-owned path 只跑 Android，共享
graphical/platform adapter 跑受影响端，共享
build/resource/toolchain contract 跑全矩阵，普通 backend-neutral `src/multiplayer_*` 不触发 package baseline。
`Makefile` 只选择实际使用它的 Linux package；root `CMakeLists.txt`/`src/version.cmake` 由 Linux production workflow
定向 configure 并构建 `get_version`，不触发无关平台 package。public transport/crypto header、protocol source、
generated header 或 schema 当前临时映射到 Windows/Android actual-source package，等轻量 production portability
target 建立后再缩小成本。该 fallback 即使实际执行完整 package，也只按本次 Tier 2 目标计作 changed production
source 的编译证据；除非 package/resource/runtime 本身是验收对象，不能反向把 package 全绿变成规范要求。
自动 selector 无法可靠解析 base/diff 时应快速失败并要求显式 manual target，不得静默消耗全矩阵。
transport/protocol workflow 手工默认 `linux`，自动普通 production path 也只跑 Linux production tests/process
smokes；其 Windows MSVC 与 Android NDK jobs 只编译 isolated transport spike，因此仅在 spike/workflow 自身变化或
显式 manual target 时运行，不能被描述为 changed production source 的平台证据。public protocol/header 的 W/A
证据当前来自 baseline actual-source package，而不是该 spike；后续应以轻量 production subset target 替换重包。

## 21. 可观测性与性能

### 21.1 结构化日志

每条 server 日志包含：

- timestamp
- severity/category
- server instance/world ID
- session ID/player ID（必要时脱敏）
- turn/revision
- command type/client seq
- duration/result/error code

不得记录 invite code、token、private key 或完整人物包。

### 21.2 关键指标

- 在线/认证中/断线宽限中的连接数。
- command queue depth 与拒绝数。
- 每 turn 各阶段耗时。
- scene/snapshot 构建和压缩耗时。
- 每玩家入站/出站字节。
- full snapshot 频率。
- save 时长、失败、generation 数。
- barrier 等待每名玩家的时间。
- 内存中的 submap、monster、NPC、item 和 snapshot 大小。

### 21.3 初始性能预算

这些是工程预算，不是未测量的承诺：

- 4 玩家共享 bubble 时，普通 turn server p95 小于 100 ms，不计玩家等待。
- LAN command 到确认 p95 小于 100 ms。
- 常规压缩 delta 中位数小于 64 KiB。
- 长期 full scene/avatar snapshot 目标控制在 2 MiB 压缩后并支持 chunk。Phase 2 当前先执行更严格的
  512 KiB 未压缩 scene payload 上限，超限时从外圈缩减可见半径并保留中心/player anchor；这是移动端
  安全门禁，不替代后续 delta/chunk/zstd 和明确的 reduced-viewport metadata。
- snapshot 构建不得让模拟线程产生超过 50 ms 的额外停顿；超出后需要增量和缓存。
- 慢客户端不能拖垮其他客户端，发送队列达到上限后断开慢连接。
- server transport 的 admission slot 绑定完整 logical connection lifecycle：socket 关闭后仍占用 slot，直到
  simulation thread 消费该连接的 terminal event。frame queue 必须为每个 admitted lifecycle 保留 connected 与
  terminal control capacity，使 connect/reset churn 无法复用 slot 并挤掉较早 terminal event。若理论上不可达的
  control enqueue failure 仍发生，transport 必须在停止前持久发布独立于普通 inbound queue 的全局 fatal detail。

## 22. 分阶段实施路线

### Phase 0：基线、ADR 与可行性验证（2 至 4 周）

- 固定上游基线与 fork 分支策略。
- 建立 ADR：authority、time model、transport、protocol、player bridge、rendering、save、character policy、bubble policy。
- 为 `game::do_turn()` 建立阶段级测试和 profiling。
- 实现两个 avatar 同图存在的 test harness。
- 验证稳定 avatar ownership、活动上下文指针和 player-runtime sidecar；保留完整 move-swap 失败对照。
- 审计 `get_avatar()` 在 world phase 中的高风险调用。
- 验证 Asio/TCP 在 Linux、MSVC、Android NDK 编译。
- 验证 TLS 方案或明确 LAN/VPN 限制。

退出标准：角色桥接方案作出 go/no-go 决策；Android/Windows/Linux 的 transport spike 通过；没有把 POC 代码直接当生产架构。

Phase 0 已于 2026-07-12 按证据关闭：稳定地址 bridge 在 GCC release 和 ASan/UBSan/LSan 下完成
274/274 正向 assertions，完整 move-swap 保留 3 个预期失败对照；hosted baseline run `29205262759`
验证 Linux、Android arm64 和 Windows MSVC artifacts；transport run `29205262750` 验证 GCC 13、
Clang 18、MSVC 和 Android NDK arm64。ADR-0003 与 ADR-0005 已接受。嵌入式 TLS、生产 transport、
canonical generation save 和实际远程命令均未被 Phase 0 spike 冒充完成；项目当时由此进入 Phase 1。

### Phase 1：Headless 运行模式与协议骨架（3 至 5 周）

- 增加 `runtime_mode` 和 server CLI/config。
- server 跳过所有 UI/SDL/sound 初始化。
- 增加 transport、frame codec、FlatBuffer schema、版本握手和限流。
- 增加 content manifest。
- 建立 in-process loopback client。
- 增加结构化日志和 shutdown signal handling。

退出标准：服务器可无终端交互启动、监听、握手、拒绝不兼容客户端并优雅退出。

截至 2026-07-13，Phase 1 已按本地与 hosted 证据关闭：生产有界 Asio transport、固定 FlatBuffers
schema/handshake、真实 ordered content SHA-256、严格 config/token、headless world bootstrap、JSON log、signal
save/shutdown 和真实进程 loopback 均已验证；baseline run `29219328448` 与 transport/protocol run
`29219953446` 分别覆盖 Linux package、Windows MSVC tiles+sound、Android arm64 APK，以及 GCC 13、Clang 18、
MSVC 和 Android NDK gates。后续改动按第 20.6 节选 Tier 1/2/3，不能复用 Phase 1 run 冒充新代码证据，也不再
要求每个 backend-neutral shared C++ 提交都完成全平台 package；当前 Phase 2 batch 的对应结果记录在下节。

### Phase 2：单远程玩家垂直切片（已于 2026-07-14 关闭）

- 拆出 input resolver 与简单 command executor。
- 单人模式改为调用同一个 move/wait executor。
- 服务器执行远程 move/wait。
- 构建最小 visible scene：terrain、furniture、player、monster。
- Windows 客户端连接并使用本地键位/tileset 渲染。
- Android 客户端完成同样 smoke test。
- 实现 full snapshot、revision、command result 和 ping。

退出标准：一个客户端可以远程控制服务器唯一 avatar，画面不依赖服务端图形环境。

当前服务器侧纵向切片已完成 wait/move shared executor、simulation-thread remote turn callback、terrain/
furniture/player/monster visible scene、full snapshot、revision、command result、ping、resync、resume 和幂等重放。
客户端侧现有生产 Asio transport/state machine、严格 auth/resume/revision/sequence 边界、桌面 `--connect`、本地
keys/touch → semantic wait/move、curses fallback 和 `cata_tiles::draw_remote_scene()`；scene payload 受 512 KiB
预算约束，客户端有 30 秒 ping/120 秒 deadline、manual resume/fresh retry，以及等待既有 command settlement 后
按 simulation FIFO 排入 ACK、待 write drain/ordered-close 精确完成才清除 resume record 的 typed
`DisconnectNotice` session release。服务端/客户端还验证了共享 256-command replay window、跨类型 sequence
复用拒绝和 pending-scene heartbeat；服务端 logical connection slot 现会保留到 terminal event 被消费，防止
connect/reset churn 挤掉 terminal control event，防御性 control-enqueue fatal detail 也独立持久化。真实 Linux
PTY 的最终 release 与 sanitizer binary 均已完成 auth/scene/断线/resume/replay/move/quit smoke，完整
`[multiplayer]` sanitizer suite 也无 ASan/UBSan/LSan/stack-use-after-return finding。Android arm64/x86_64 debug
APK 已编译 Java connect 表单和 SDL renderer。source `6403a949fb537be14ec4d5757f295adbf5c5f99b` 的 baseline
run `29303564150` 与 transport/protocol run `29303564152` 均为 terminal `success`，已关闭本批 Linux、Windows
MSVC 和 Android package/compile platform gate。随后 KVM-backed API 35 x86_64 emulator 已取得 clean launch、
auth/render、单次受控 wait/move、Activity pause/resume、强制 network disconnect/reconnect 和 clean
save/shutdown 的本地绿色证据；先前 `-accel off` 的 ANR/held-touch flood 只保留为不计入门禁的诊断历史。

该设备运行同时暴露了 display-derived build ID 的跨后端握手缺陷：同源 Android SDL3 client 曾发送
`90e5fa3+SDL3`，headless server 则发送 `90e5fa3`。commit
`e078eb6aef25a9cc72eea45793114c931a52b896` 已改为第 13 节定义的 backend-neutral canonical ID，并用该完整
SHA 重跑 Android lifecycle 成功；该 runtime 证据精确属于 `e078eb6`。同一提交的 baseline run `29328086046`
和 transport/protocol run `29328086326` 均为 terminal `success`，已覆盖 Linux、Windows MSVC、Android package/
compile 以及生产 tests/process smoke。

随后只读审计发现 CMake target 可能因已有 `version.h` 跳过重算且未把 override 传入实际生成、Make 会把
`git diff` 的 rc > 1 错误误当作 `-dirty`，以及旧 Windows workflow 使用大小写不敏感的 `-notcontains`。实际
`e078eb6` Windows artifact 输出是小写 canonical ID 并通过当时门禁，但旧断言本身不提供大小写敏感保证。pushed
commit `86336ea847bea45f727fd97d74a811a32712518c` 已改为 always-run CMake target、Make rc > 1 fail closed 和
PowerShell `-cnotcontains`。baseline run `29330811779` 的 7 个 jobs 与 transport/protocol run `29330811746`
的 3 个 jobs 均为 terminal `success`，验证 Linux、Windows MSVC、Android package/compile、大小写敏感 Windows
assertion、生产 tests/process smoke 和 transport gates。至此 Phase 2 的 single-remote-player、hosted platform、
canonical build identity 与 Android lifecycle exit criteria 均有记录证据，Phase 2 于 2026-07-14 正式关闭。
完整 lifecycle polish、remote avatar replica 与更完整 scene layers 仍属于后续 Phase 4，不能因阶段关闭而视为完成。

### Phase 3：第二玩家与共享 Scheduler（5 至 8 周，Gate 2 single-root production lifecycle 已关闭）

ADR-0002 所需的首轮 source/ownership audit 已完成，未发现需要推翻 accepted ADR 的冲突。Phase 0 已经提供
地址稳定的 player registry/runtime、active-player guard 和额外 human tracker/query/map-shift 基础。Gate 1/2 已把
session ownership、two-phase admission、selected-root lifecycle、scheduler/adapter 和 actual legacy world routing
组合进 production single-root server；ADR-0010/0011 已接受。当前任务转为构建 multi-runtime owner 与关闭多人规则
矩阵，不重新创建这些组件，也不把 single-root owner 直接放宽为多玩家。`players.max` 仍必须为 `1`，Gate 2 关闭不
等于 Phase 3 退出或 production two-player routing 完成。

- production session/runtime directory 已成为 canonical generation owner，runtime 由 directory 精确推进，lobby 只保留
  受控 mirror，并把 auth/resume 落为 `pending -> plan -> pre-encode -> directory commit -> transport enqueue -> lobby
  mirror/event publish`；runtime exact transition、protocol
  minor `1` 的 last accepted generation、同 fingerprint lost-response replay、exact-tuple confirmation、fresh/terminal
  token 回收和 ordered rejection 已有 Linux 单元证据。accepted enqueue failure 只清 binding、不回滚 generation；
  resume 可以用 directory fingerprint 同代重放并修复暂时落后一代的 lobby mirror，fresh auth 则没有同代 replay
  承诺。
- **Gate 1：selected-root lifecycle contract/API（已关闭，source
  `cbd19b48d652be735a3c83fe841d2d7c831aecba`）。** 状态表与 real directory/runtime/scheduler 组合已定义并验证
  transport disconnected、graceful release pending、barrier disconnected grace、forced-wait pending、terminal
  barrier、world completed、runtime offline、server dormant、same-runtime resume reactivation、same-generation replay
  和 unpublished `+1` repair。owner-bound ticket/lifecycle-version-bound recipe、guard/save/fault gate、timeout/resume
  单赢家和 caller-reported ACK-queued/early-close 顺序已有 Linux focused/full/sanitizer 证据。该 gate 只证明
  contract/API；production dormant 由下述 Gate 2 source 证明。
- **Gate 2：production single-root lifecycle（已关闭，source
  `2e9236c7bf91782ad3f15daa5d8aa0e3929f355a`）。** `multiplayer_single_root_owner` 私有持有 directory、scheduler、
  lifecycle 与 adapter，并接通 `disconnect -> grace -> forced wait -> terminal -> exact actual world -> player-end ->
  offline -> dormant` 及 same-runtime resume。`do_turn_remote_owned()` 提供唯一 bookkeeping、mandatory player-phase
  completion、exact-once world thunk 与不可伪造 completion proof；scene/save/next turn 位于 lifecycle completed boundary
  之后。ADR-0011 的 outer/active-player-input 双层泵允许 initial/dormant admission 和 open-barrier resume，而
  open-turn resume/resync 只重放 immutable completed scene。graceful ACK 使用 typed queued/fallback/stale/fatal
  transport result，terminal close 优先于同批 queued command。open turn 或 canonical state 不可证明时 typed
  fatal/no-save，clean boundary/dormant 才允许 canonical save。Linux full `[multiplayer]`、定向 sanitizer、headless
  process、native curses PTY 和 open-turn save-hash regression 已记录；本 gate 未改变 wire/schema/platform-owned
  code，因此按第 20.6 节未运行 Windows/Android。
- **Gate 3：two-runtime owner 与规则矩阵。** 首个 inner owner-contract sub-gate 已由 source
  `2eccb92087991966423c18b63f6ef707462b1428` 关闭：in-process two-runtime wait-only case 已提升为 owner-level
  integration，immutable roster 的 pinned runtime/avatar identity、persistent fairness、explicit player-end pending receipt
  和 fail-stop 已有 Linux release/sanitizer 证据。第二个 inner rule sub-gate 已由 source
  `3204f8f45606a20ea6ab0892369b9806f89c4353` 关闭：thin router、verified adjacent empty-ground move、active-avatar plain-walk seam 和 exact
  move/move/wait/wait position/moves/action-bookkeeping isolation 已有 Linux release/full-multiplayer、定向 sanitizer 与
  native curses PTY 证据。第三个 inner rule sub-gate 已由 source `84d8ca056bf72ed9776890f7ca4212a3bfdf7c2e` 关闭：adjacent semantic move
  的 typed human block、exact blocker、symmetric retry 和两 turn contested-tile winner rotation 已有 Linux
  release/full-multiplayer/定向 sanitizer 证据；该 slice 无 production caller，因此未重复 process/PTY。Gate 3 仍继续
  拒绝外部 `players.max > 1`。下一步依次关闭 monster target/attack 与 death/game-over、field/scent/NPC、tether/group
  shift，以及 messages/safe-mode/stats/player-scoped cache isolation。全部 owner/rule gates
  绿色后，先开放仅供 integration/process test 使用的双连接 routing 并完成 Linux 双 client smoke/soak；该测试开关
  不得作为用户配置发布。只有这些证据也绿色后，才评估允许用户配置 `players.max > 1`。

后续 Gate 3 编辑循环继续只跑 Linux incremental/focused tests；改变 shared-turn/authority invariant 的切片收口按
风险增加完整 `[multiplayer]` 和 sanitizer，真实双 client routing 接通时再增加 Linux process smoke。只有已完成
批次修改公共 wire/ABI/toolchain 或明确的 Windows/Android owned boundary 时，才补受影响平台证据。

退出标准：两个 test client 能在同一 bubble 中连续游戏 60 分钟，怪物可正确攻击任一玩家，ASan/UBSan 无错误。
Phase 3 内部 scheduler/rule/session 切片默认由 Linux headless server + native client 收口；阶段出口对当前候选运行
第 20.6 节规定的 Linux 主门禁。Windows/Android 仅在 Phase 3 修改对应 owned code、公共 wire/ABI/toolchain 边界或
改变平台产品声明时补当前候选证据；否则记录最近兼容 evidence commit/run 与到当前候选的 diff audit，不得称为
当前候选已在该平台编译。若未改变 Android lifecycle，无需重复 Phase 2 的完整设备 lifecycle 剧本。

### Phase 4：客户端 replica 与渲染完整度（6 至 10 周）

- 完善 remote scene layers：items、fields、traps、vehicles、lighting、special vision、overlays。
- 添加 avatar replica 与本地 panels。
- 添加 per-player messages、sound events、weather animation。
- 添加 overmap known-state 模型。
- 客户端 connect/recent servers/compatibility UI。
- 完善 Android pause/resume、后台超时、进程重启 resume checkpoint 与自动 reconnect/backoff；Phase 2 只要求
  单客户端 vertical-slice 的最小真实设备 lifecycle smoke。

scene/replica/rendering 的编辑循环和普通切片先用 Linux headless server + Linux SDL client 验证。Windows renderer/
input/UI 形成关键批次后，在该批收口候选上运行 MSVC/native 或 package gate；Android touch/Activity/reconnect 形成
关键批次后运行目标 APK/resource 与 emulator/device gate。不得为同一未收口 scene layer 的每个中间提交重复两端
完整构建，但平台批次通过后若继续修改其 owned boundary，必须对新候选重跑。

退出标准：移动、战斗和观察体验与同版本本地单人模式视觉上基本一致，不向客户端泄露隐藏实体；当前 Linux 候选
完成完整 visual/visibility 回归，Windows/Android 按本阶段实际完成的平台批次与产品声明补证据，未变化的平台可按
第 20.6 节引用最近兼容 evidence commit + diff audit。

### Phase 5：存档与人物导入（4 至 8 周）

- 拆分 world runtime 与 player runtime serialization。
- 保存 RNG engine state。
- 多玩家 barrier save/restart。
- portable character schema/importer。
- 实现 `server_owned` 与 `copy_in`，随后实现 `portable_lease`。
- ID/item UID 重映射及 world reference 清理。
- generation save 与恢复策略。

退出标准：服务器重启后多人状态一致；旧单人存档仍可加载；重复/陈旧人物 generation 被拒绝。

### Phase 6：核心玩法动作覆盖（12 至 24 周）

按动作组逐批迁移，每批都要求单人和联机共用 executor：

- 相邻交互与拾取/丢弃。
- 穿戴、使用、进食、阅读、装填。
- 射击、瞄准、投掷、法术。
- 制作、拆解、建造、屠宰、睡眠。
- advanced inventory 与 zones。
- NPC 对话、任务、交易。
- 车辆驾驶、控制、维修和乘客。
- computers、特殊活动和 mod EOC 流程。

每个动作必须有 command schema、server validation、client UI、错误处理、保存/重连测试和两玩家冲突测试。每个
动作家族的编辑循环和功能收口默认使用 Linux headless server + Linux native client；不因动作数量或中间提交数量
重复 Windows/Android 完整构建。若一个收口批次修改 public wire schema/header，则在该批冻结候选上补一次实际编译
production source 的 MSVC/NDK 定向 gate；若修改平台 input/UI/renderer/lifecycle，则只补受影响平台的 Tier 2 gate。

退出标准：预先维护的 action coverage matrix 达到 v1 门槛，未支持动作在客户端被明确禁用而不是导致 server popup 或挂死。

### Phase 7：安全、运维与平台发布（4 至 8 周）

- 完成 TLS/外部隧道发布策略。
- invite/account/session resume。
- admin command 与审计。
- rate limiting、fuzz、恶意人物包测试。
- Windows/Android 安装包和 Linux server 包。
- server config migration、backup、restore 文档。
- 4 客户端 6 至 12 小时 soak。

退出标准：可供非开发者部署私服；默认配置不裸露不安全公网服务；故障有可诊断日志。

### Phase 8：多 Reality Bubble（可选，12 至 24 周）

- 引入 `bubble_runtime` 与 manager。
- map/tracker/scent/sounds 迁移。
- bubble merge/split 和实体所有权。
- 全局 turn 协调。
- 任意距离玩家和跨 bubble 重连。

退出标准：两个玩家相距多个 overmap tile 仍可独立行动，接近后 bubble 合并且无重复实体/重复处理。

## 23. 建议的文件级改动

### 23.1 现有文件

| 文件 | 计划改动 |
| --- | --- |
| `src/main.cpp` | runtime mode、server/client CLI、跳过 headless UI 初始化 |
| `src/game.h` | player registry/context、scheduler API、逐步移出 player state |
| `src/do_turn.cpp` | 按阶段拆分主循环，多玩家 barrier |
| `src/handle_action.cpp` | input/command/executor 分层，动作逐批迁移 |
| `src/game.cpp` | human player 查询、active guard、shared_from/critter_by_id |
| `src/multiplayer_player_registry.*` | stable runtime/avatar ownership、ID/position index、排除 mover 的 exact human occupancy lookup |
| `src/creature_tracker.*` | 额外 human avatars 与位置索引 |
| `src/current_map.*` | 扩展为 simulation context 的基础 |
| `src/map.*` | 多玩家 shift、scene builder 支持、后续 bubble scope |
| `src/cata_tiles.*` | `draw_remote_scene()` 与本地 scene adapter |
| `src/messages.*` | per-player message sink 与网络事件 |
| `src/sounds.*` | bubble sound store 与 client sound events |
| `src/savegame.cpp` | world/player runtime schema 拆分、RNG state |
| `src/savegame_json.cpp` | portable avatar 字段和迁移支持 |
| `src/game_io.cpp` | server multi-player save/load/generation |
| `src/worldfactory.*` | headless config world bootstrap/spawn |
| `src/options.*` | option scope 元数据，server/world 与 client-only 分离 |
| `src/main_menu.*` | Connect/Recent Servers/Choose Character UI |
| `CMakeLists.txt`、`src/CMakeLists.txt` | server target、网络依赖、协议生成验证 |
| `android/app/src/main/AndroidManifest.xml` | INTERNET permission |
| `android/app/jni/CMakeLists.txt` | 网络/TLS 依赖 |
| `.github/workflows/*` | server/client/Android/integration/fuzz CI |

### 23.2 建议新增文件

保持仓库当前扁平 `src` 风格，先使用统一前缀：

```text
src/multiplayer_protocol.fbs
src/multiplayer_protocol_generated.h
src/multiplayer_transport.h/.cpp
src/multiplayer_connection.h/.cpp
src/multiplayer_server.h/.cpp
src/multiplayer_client.h/.cpp
src/multiplayer_session.h/.cpp
src/multiplayer_player_registry.h/.cpp
src/multiplayer_player_context.h/.cpp
src/multiplayer_turn_scheduler.h/.cpp
src/multiplayer_single_root_owner.h/.cpp
src/multiplayer_multi_runtime_barrier_owner.h/.cpp
src/multiplayer_multi_runtime_command_router.h/.cpp
src/multiplayer_command.h/.cpp
src/multiplayer_command_executor.h/.cpp
src/multiplayer_snapshot.h/.cpp
src/multiplayer_scene.h/.cpp
src/multiplayer_character_package.h/.cpp
src/multiplayer_content_manifest.h/.cpp
src/multiplayer_server_config.h/.cpp
src/server_main.cpp

tests/multiplayer_protocol_test.cpp
tests/multiplayer_scheduler_test.cpp
tests/multiplayer_player_slot_test.cpp
tests/multiplayer_multi_runtime_barrier_owner_test.cpp
tests/multiplayer_multi_runtime_command_router_test.cpp
tests/multiplayer_scene_test.cpp
tests/multiplayer_character_import_test.cpp
tests/multiplayer_save_test.cpp
tests/multiplayer_integration_test.cpp
```

当模块稳定且 CMake 改为显式 source list 后，再考虑迁入 `src/multiplayer/` 子目录，避免一开始同时引入目录和构建系统大改。

## 24. PR/提交拆分建议

建议保持每个 PR 按第 20.6 节对应频率和层级可独立验证并尽量不混入格式化。“可独立验证”表示当前 Linux 候选
闭环及该 PR 实际触及的平台/公共边界有证据，不表示每个 PR 或每个中间提交都完成全平台矩阵。平台专属改动应按
可验收的关键批次组织，使该批收口时只需对冻结候选运行一次相应平台门禁：

1. 加入本计划、ADR 模板和 multiplayer feature flag，不改行为。
2. runtime mode 与 headless bootstrap。
3. transport/frame codec/loopback tests。
4. protocol handshake/content manifest。
5. `do_regular_action` 中 move/wait executor 提取，单人行为不变。
6. minimal remote scene 与 Windows 单客户端。
7. Android connect smoke test。
8. human player registry 与 creature tracker 扩展。
9. active player guard 与 slot invariant tests。
10. multiplayer turn scheduler，两玩家 move/wait。
11. per-player messages/safe mode/stats 基础隔离。
12. shared bubble shift/tether。
13. world/player save schema 拆分。
14. portable character importer。
15. reconnect/idempotency/full snapshot recovery。
16. 按动作家族逐 PR 迁移。
17. TLS/auth/admin/ops。
18. release CI 与 soak hardening。

通用、与联机无关且有独立价值的重构可以尝试先向上游提交；multiplayer-specific code 应保留在 fork。不要把大量 multiplayer 条件散布在业务文件中，优先放在 router/context/adapter 边界。

## 25. 风险登记

| 风险 | 严重度 | 缓解措施 |
| --- | ---: | --- |
| 活动玩家切换破坏指针/身份 | 致命 | avatar/runtime 地址稳定；持久 ID/UID handle；Phase 0 sanitizer 与 identity gate |
| 世界阶段误用“当前活动玩家” | 致命 | phase audit、debug activation reason、多人对称测试 |
| UI 与规则耦合导致 server 阻塞 | 致命 | typed query/commit；server UI 调用断言；按动作覆盖矩阵迁移 |
| 多玩家时间体验差 | 高 | turn barrier、可配置 timeout、全员安全快进、明确暂停状态 |
| 客户端人物复制/作弊 | 高 | server policy、portable lease、generation/signature、默认 server-owned 可选 |
| 存档多文件崩溃不一致 | 高 | barrier save、generation、manifest-last、备份与回退 |
| Android TLS/依赖打包困难 | 高 | Phase 0 独立 spike；transport abstraction；LAN/VPN 受限退路 |
| 隐藏状态泄露 | 高 | server scene builder、visibility golden tests、不发完整 submap |
| action coverage 工作量失控 | 高 | 垂直切片、优先主流程、未支持动作显式禁用、DoD 模板 |
| 上游高频变动导致 merge 成本 | 高 | additive adapters、小 PR、固定前缀、每 1 至 2 周集成上游 |
| 多 reality bubble 重复模拟 | 致命 | v1 后置；唯一 submap ownership；merge/split invariant tests |
| 慢客户端拖垮服务器 | 中 | bounded send queue、delta、chunk、超限断连 |
| mod/data 不一致 | 高 | ordered content manifest/hash，严格拒绝混连 |
| 本地语言与服务器字符串不一致 | 中 | 逐步结构化消息 ID；短期 UTF-8 fallback |

## 26. 发布验收标准

### 26.1 功能

- Linux headless server 从配置创建世界并监听。
- Windows 与 Android 客户端可同时直连。
- 两端各自使用本地键位、触控、tileset、字体、语言、面板和 soundpack。
- 两名玩家可互相看见、阻挡、攻击、治疗和交换物品。
- 怪物/NPC 能正确感知并攻击任一玩家。
- 核心动作 coverage matrix 达到发布门槛。
- 制作、阅读、睡眠等活动遵守共享时间策略。
- 断线/重连不会重复执行最后命令。
- 服务器重启后世界和所有人物恢复。
- Portable lease 不接受陈旧 generation。
- 不兼容版本/mod 提供明确拒绝原因。

### 26.2 正确性

- 同一世界内 `character_id` 唯一。
- 有效 `item_uid` 在服务器 world 中唯一。
- 同一实体只属于一个 tracker/bubble。
- 一条 command 最多执行一次。
- 客户端看不到服务端判定不可见的怪物、陷阱和地图内容。
- 单人旧存档 round-trip 无新增损坏。
- 多玩家 save/load 状态 hash 一致。

### 26.3 稳定性与安全

- ASan/UBSan 集成测试通过。
- 4 客户端 6 小时 soak 无 crash、deadlock、无限队列增长和存档损坏。
- 断网、kill -9、磁盘写失败均能恢复上一有效 generation。
- Fuzz corpus 无已知 crash。
- 默认公网监听不允许 plaintext。
- 日志不泄露认证密钥与人物包。

## 27. 最先执行的 90 天计划

第 1 至 2 周：

- 建 fork 分支与 CI 基线。
- 写 ADR。
- 建稳定 avatar 上下文、full-swap 失败对照和 player-runtime sidecar spike。
- 建 Asio Linux/Windows/Android 编译 spike。

第 3 至 5 周：

- 实现 headless runtime mode。
- 实现 frame codec、handshake、content manifest。
- 建 loopback test client。

第 6 至 8 周：

- 提取 move/wait executor。
- 单远程客户端控制服务器唯一 avatar。
- 最小 visible scene + Windows tiles 渲染。

第 9 至 10 周：

- Android connect/render smoke test。
- revision、idempotency、disconnect resume 基础。

第 11 至 13 周：

- player registry、active player guard。
- 两玩家共享 scheduler。
- creature tracker、阻挡、近战和怪物目标。

90 天退出目标不是“完整联机”，而是形成一个架构正确的双玩家垂直切片：两端本地渲染、服务器权威、同一 bubble、可移动和战斗、可断线恢复，并且单人模式仍正常。

## 28. 长期维护策略

- 保留 `upstream` remote，fork 的 `origin` 指向自己的仓库。
- multiplayer 主分支通过第 20.6 节按 diff 风险要求的当前层级 CI 后再吸收上游；不要求普通 shared-code 合并先跑
  无关平台完整矩阵。
- 每 1 至 2 周合并一次上游，不积累数月差异。
- multiplayer 文件使用统一前缀，减少与上游文件名冲突。
- 对 `do_turn.cpp`、`game.h`、`handle_action.cpp`、savegame 文件的改动保持小块、可审查。
- 禁止无关格式化、批量 rename 和目录重排。
- 每次上游合并后优先在当前 Linux 候选上跑 single-player save compatibility 与 two-client integration。只有合并
  diff 触及 Windows/Android owned code、公共 wire/ABI/toolchain、platform conditional 或相关产品声明时，才补受影响
  平台；否则在阶段出口记录最近兼容 evidence commit 与从该提交到当前候选的 diff audit。
- 协议版本、server state schema、portable character schema 分开演进，不能复用 `savegame_version` 表示所有兼容性。
- 保持 action coverage matrix、known limitations 和 migration guide 与代码同 PR 更新。

## 29. 明确不要采用的捷径

- 不让多个进程共享同一个 save 目录。
- 不做跨平台 deterministic lockstep。
- 不把客户端整个 `.sav` 当作服务器人物。
- 不每回合发送完整世界或完整 submap。
- 不信任客户端提交的行动结果。
- 不用枚举整数或容器下标作为长期网络 ID。
- 不在模拟对象上加锁后从多个线程同时执行游戏规则。
- 不让服务器为每个菜单阻塞等待按键。
- 不先重写所有 UI 或所有 `get_avatar()` 调用。
- 不在首个版本同时做多 reality bubble、NAT traversal、公共账号和自动 mod 分发。
- 不自行设计加密协议来替代成熟 TLS/VPN。

按上述顺序实施，可以把最危险的改动集中在少数边界：活动玩家上下文、回合调度、命令执行、可见场景、存档拆分和人物导入。地图生成、物品规则、战斗公式、NPC/怪物主体逻辑、JSON 数据、客户端 tileset 与输入系统均尽量复用现有实现。这是达到目标同时控制长期 fork 成本的最小改动路径。
