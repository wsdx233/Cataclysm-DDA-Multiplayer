# CDDA 多人 fork 当前状态

- 更新日期：2026-07-14
- 分支：`multiplayer/main`
- 当前阶段：**Phase 3，shared scheduler 与当前仅由测试调用的 source phase adapter/seams 已落地；下一入口是 production session/runtime directory**
- 上游基线：`d84b90dd2aee090ca28c8dad5cdf1fab6dea151a`
- 当前 scheduler source 提交：`45be077ac2d0d042190e54d1bdb77d48676b67e6`（transport 全绿；baseline 的 Linux、Android、resources 成功，Windows validator failure）
- 上一个完整 hosted platform gate 全绿提交：`86336ea847bea45f727fd97d74a811a32712518c`（canonical build-ID hardening）
- 当前 Phase 3 slice：scheduler execution-fault latch、`multiplayer_turn_phase_adapter`、forced wait mode、单人
  action/world helper seams，以及 Linux release/sanitizer tests；adapter 尚未接入 production runtime
- 当前 Windows 定向修复：`c9b28086e973fa497d5bd9f9a37e64a9ac22e464`；hosted MSVC package 已完整成功
- 上一个 hosted-green selector source：`9a561f6172d9042c1c424f16d923448e0de9152a`；当前 source 进一步把
  transport routine path 收敛到 Linux，并移除 automatic classification failure 的隐式全矩阵 fallback
- 当前下一行动：从 `src/main.cpp`/`src/multiplayer_server_lobby.*` 建立统一 generation 的 authoritative
  session/runtime directory，先完成 two-phase auth/resume 与 active-root lifecycle 决策，再以 `players.max = 1`
  接入 production scheduler/adapter 和 actual claimed bubble

## 当前结论

Phase 0 已关闭，ADR-0001 至 ADR-0009 继续有效；ADR-0010 已记录 authoritative session directory/two-phase
admission 方向，状态为待验证。Phase 1 的本地门禁和 hosted 平台门禁都已取得绿色证据：生产 dedicated runtime、Asio transport、FlatBuffers 协议、严格握手/content manifest、token auth、canonical server-owned avatar、结构化日志、signal save/shutdown 和真实进程 command/resume smoke 均已验证。

Phase 2 的服务器侧单远程玩家纵向切片保持完成。本批又实现了可复用的生产客户端 transport/state machine、桌面/Android connection UI、本地 input → semantic wait/move、visibility-filtered scene → 本地 curses/tiles renderer、heartbeat/manual reconnect、断线 resume、exactly-once replay 和按 simulation FIFO 完成的 typed clean session release。最终 source 的普通 release 与 ASan/UBSan binary 都已通过真实 Linux PTY auth、scene、wait、强制断线、resume、未确认命令重放、move 和 clean quit。

本批提交 `6403a949fb537be14ec4d5757f295adbf5c5f99b` 的 hosted platform build gate 已关闭：baseline run
`29303564150` 和 transport/protocol run `29303564152` 均为 terminal `success`，覆盖 Linux curses、Windows x64
MSVC tiles+sound、Android arm64 release/x86_64 debug、生产 Linux process/client smoke、standalone GCC/Clang/MSVC
transport 和 Android NDK arm64。

KVM-backed API 35 x86_64 emulator 已取得本地绿色 lifecycle 证据：clean SplashScreen startup、remote scene
render、单次 wait、单次 move、HOME pause/Activity resume、强制 TCP disconnect、airplane mode route recovery、
同一 identity 两次 resume、post-resume commands、clean disconnect 以及 server save/shutdown。该运行还暴露了旧
display-derived build identity 在 Android SDL3 client（`90e5fa3+SDL3`）与 headless server（`90e5fa3`）之间的
握手不一致；commit `e078eb6aef25a9cc72eea45793114c931a52b896` 已改为 backend-neutral canonical full Git
SHA，并用该 ID 完整重跑成功。该提交的 baseline run `29328086046` 与 transport/protocol run `29328086326`
均为 terminal `success`，覆盖 Linux、Windows MSVC、Android package/compile 和生产 tests/process smoke。

后续只读审计又发现 CMake 已有-header/override、Make dirty probe rc > 1 和 Windows assertion 大小写三个缺口；
pushed commit `86336ea847bea45f727fd97d74a811a32712518c` 已修复。baseline run `29330811779` 与
transport/protocol run `29330811746` 均为 terminal `success`，分别 7/7 和 3/3 jobs 全绿。

**Phase 2 已于 2026-07-14 正式关闭。** Android KVM lifecycle 证据精确属于 `e078eb6`，没有为 `86336ea`
重跑；`86336ea` 的 hosted runs 关闭后续 build-generator hardening gate。

Phase 3 的首轮 source/ownership audit 已完成，未发现需要推翻 ADR-0002 的冲突。Phase 0 的地址稳定
`multiplayer_player_registry`/runtime、`multiplayer_active_player_guard` 和 human tracker/query/map-shift 支持已经
存在；后续工作是接入 production session/scheduler，而不是新建第二套 registry/context。audit 同时确认五个
`do_turn()` trace labels 只是观测点：`player_begin`、`player_input`、`world`、`player_end` 都仍混合 per-player、
world-once 或 local-UI 工作，不能按 label 机械变成 ownership boundary。

当前 Phase 3 source slice 新增首个纯 `multiplayer_turn_scheduler` policy/test。它实现 immutable roster
snapshot、稳定首位轮转、typed action disposition、generation resume、current-only timeout、
`automatic_wait_pending` 和 `world_ready -> world_processing` ticket 状态语义，但没有接入 production
`src/main.cpp`/`do_turn_remote()`，也不拥有 avatar、socket、command payload 或 gameplay callback。服务器仍只有
一个 `active_remote_session`，`players.max` 继续只能是 `1`；不能把这个 policy 或其单测描述为已完成两玩家 server。

当前第二个 Phase 3 slice 在此 policy 上增加了 source `multiplayer_turn_phase_adapter`（目前仅由测试调用）和保持单人
行为的 source seams：

- adapter 只允许 simulation thread，并精确验证 current slot/state、player ID、session generation、registry
  ownership、runtime active 状态、构造时 root active context、guard engagement 和退出恢复。
- accepted player callback 与 authoritative forced wait 都先在目标 guard 内执行真实规则和目标 avatar action
  bookkeeping，退出 guard 后才记录 scheduler。forced wait 绕过玩家请求的 safe-mode permission，但仍执行真实
  `Character::pause()`；`check_safe_mode_allowed()` 的 active-avatar permission gate 已避免 alpha 状态污染 beta，但
  character rules/whitelist 等全局 safe-mode state 尚未完成玩家隔离。
- callback/exception、wait/world failure、context restore failure 或规则执行后的 scheduler record failure 会永久 latch
  scheduler execution fault；replacement adapter、新 turn 和其他 transition 都被拒绝，避免重试非幂等副作用。
- `do_turn.cpp` 已命名 `record_turn_player_action()` 和 `process_legacy_single_player_bubble_turn()`，并有 first-turn
  remote phase-order/moves 回归；但 `do_turn_impl()` 仍直接执行 actual bubble，adapter world test 只是计数 lambda。
- in-process 两 runtime wait-only barrier 已通过，但没有两个 client、production session owner 或 process-level routing。

因此本切片的准确结论是“scheduler/adapter contract 与单人 extraction seam 已在 Linux 验证”，不是“生产 world
callback、断线 auto-wait 或两玩家 server 已接入”。

该 scheduler source 已作为 commit `45be077ac2d0d042190e54d1bdb77d48676b67e6` 推送。transport/protocol run
[`29340055386`](https://github.com/wsdx233/Cataclysm-DDA-Multiplayer/actions/runs/29340055386) 已 terminal
`success`：Linux GCC 13/Clang 18 job 实际构建/测试 production source；Windows MSVC 和 Android NDK jobs 只编译
standalone transport spike，虽全绿但不属于 scheduler production-source portability evidence。
baseline run [`29340056602`](https://github.com/wsdx233/Cataclysm-DDA-Multiplayer/actions/runs/29340056602)
只有 Windows package job
[`87109602068`](https://github.com/wsdx233/Cataclysm-DDA-Multiplayer/actions/runs/29340056602/job/87109602068)
在 `Build package` 失败；Linux、Android 与 translations/tileset/shaders/soundpack resource jobs 均成功。根因是
`prebuild.cmd` 的手写 validator 误拒合法 40 位小写 SHA，不是 scheduler source 的 MSVC 编译错误。commit
`c9b28086e973fa497d5bd9f9a37e64a9ac22e464` 已改用从环境读取 ID 的 anchored、case-sensitive PowerShell regex，
并在 workflow 中增加独立 preflight 与 MSBuild immediate-exit gate。baseline run
[`29344937410`](https://github.com/wsdx233/Cataclysm-DDA-Multiplayer/actions/runs/29344937410) 的 Windows x64 MSVC
tiles+sound package job 已完整 terminal `success`，所以该 Windows-specific Tier 2 gate 已关闭。attempt 1 的
Android job 在与该 Windows 修复无关的 x86_64 debug build 阶段耗尽 hosted-runner disk；attempt 2 按用户采用的
新分层策略取消。
二者都不是 scheduler/code failure，也不阻塞 Linux-first phase adapter。Phase 3 exit gate 仍未关闭，
`players.max` 保持 `1`。

## 当前验证策略

规范性规则见重构计划第 20.6 节；本节只记录当前批次的选择。

- gameplay/phase-adapter slice 属于 **routine backend-neutral shared implementation**，选择 Tier 1：Linux GCC
  release build、focused phase-adapter/scheduler/command/turn tests、完整 `[multiplayer]` 和定向
  ASan/UBSan/LSan。adapter 尚未接 production runtime，因此本 slice 不要求 PTY loopback。
- 本批没有修改 Windows/Android-owned runtime/UI/package/lifecycle，也没有改变 wire schema、public DTO、protocol
  version 或 compiler-sensitive public ABI；Windows package 与 Android APK 按策略未运行，不是 blocker，也不是本批
  绿色证据。
- workflow/selector slice 属于独立 **Tier 3 CI-contract milestone**：自动 routine multiplayer source 只跑 Linux；
  transport Windows/Android job 只编译 isolated spike，今后仅在 spike/workflow 自身或显式 target 时运行。baseline
  自动 base/diff 无法分类时快速失败并要求 manual target，不再静默全矩阵。本地 lint/selector assertions 不是
  hosted evidence；推送后必须记录 workflow 自身触发的必要 jobs。
- 任何未编译 changed production source 的 job 不得作为该 change 的平台证据。现有 transport Windows/Android
  artifacts 只证明 standalone spike/Asio/toolchain；production public boundary 后续需要实际编译 source 的轻量
  MSVC/NDK target，或在当时显式运行对应 package。

### 分层门禁实现与验证

- 当前 source 将 transport workflow 的 manual default 改为 `linux`，增加 automatic target selector：所有匹配的
  production source 先跑 Linux，只有 `.github/workflows/multiplayer-transport-spike.yml` 或
  `tools/multiplayer/transport_spike/**` 变化才自动追加 standalone Windows/Android probes；显式 manual target 仍支持
  `linux|windows|android|all`。baseline selector 的 base fetch/diff failure 改为明确失败并要求 manual rerun。
- baseline 自动 package mapping 同时把 routine `multiplayer_transport.cpp`/`multiplayer_crypto.cpp`、server config/log
  和 mixed `main.cpp` 移出仅凭路径触发的 W/A package；public transport/crypto headers 仍触发 W/A actual-source
  package，protocol/schema/generated-header boundary 也触发 W/A actual-source package。`Makefile` 只选 Linux；root
  `CMakeLists.txt`/`src/version.cmake` 改由 transport Linux job 的定向 configure + `get_version` target 验证。generic
  file 内已有或新增平台分支变化必须人工选择 Tier 2。对应 workflow 静态/selector 验证见本批证据。

以下 commit/run 记录旧 selector 的演进与 hosted evidence；其中 fail-open 和 main/crypto/config/log 自动分类已经被
上述当前 source 取代，不再代表现行 routing：

- commit `377feba3b22f3ddafbaf259f26c4871791e9fbc6` 落地三层文档与 changed-path package selector；commit
  `6155cc9603f942ae9c4c86d69dc1d66a5fdc6a94` 将 selector 从 full-history checkout 改为 shallow checkout +
  按需抓取 event base，抓取或 diff 不可用时仍 fail-open 到全矩阵。首次 hosted selector 由此从超过一分钟的
  full-history checkout 降为约 7 秒。
- 独立复审发现 generic filename 中已有的 Win32/Android production branch、单平台手工入口和 Windows-only build
  script 分类三个缺口；commit `9a561f6172d9042c1c424f16d923448e0de9152a` 已补入 main/locale/mmap/filesystem/
  path、multiplayer crypto/transport/client UI/server config/log 等关键 platform-owned path，增加
  `workflow_dispatch target=all|linux|windows|android`，并让 PowerShell/MSVC/windist/Windows/MinGW script 只选
  Windows。路径 selector 仍只是优化；generic file 内新增 platform conditional 必须按 Tier 2 人工判断。
- 本地验证：两个 workflow 的 `actionlint` 通过；selector Bash 经 `bash -n`；Android-only、Windows-only、
  Linux+Android environment、SDL/platform shared、filesystem/locale all-platform、普通 scheduler 不触发 package、
  generic build script all-platform，以及四种 manual target 的动态分类断言均通过；`git diff --check` 通过。
- 最终 workflow source `9a561f6` 的 baseline run
  [`29353291753`](https://github.com/wsdx233/Cataclysm-DDA-Multiplayer/actions/runs/29353291753) 中，selector job
  `87154675790` 于 9 秒内成功并因 workflow 自身变化正确选择全矩阵；run 已 terminal `success`，四项 resource、
  Linux curses package job `87154803386`、Windows x64 MSVC package job `87155121109` 与 Android arm64 release +
  x86_64 compile job `87155121045` 全部成功。较早 run `29351421997` 由后续 selector 修正触发 concurrency
  cancellation；其 selector/resources/Linux 已成功，Windows/Android 被 superseding run 取消，不是代码失败。
- transport workflow rename/source `377feba` 的 run
  [`29351194814`](https://github.com/wsdx233/Cataclysm-DDA-Multiplayer/actions/runs/29351194814) 已 terminal
  `success`：Primary Linux production tests/process smokes job `87147544024`、Windows MSVC portability probe
  `87147544054` 与 Android NDK portability probe `87147544050` 均成功。结合上述 baseline，本批 selector、Linux
  production、Windows/Android portability 与完整 package matrix 均已有绿色证据；这只验证关键 CI 路由里程碑，
  不改变普通 shared-code 提交的 Tier 1 Linux-first 默认。

## 本批实现

### Phase 3 source audit 与纯 scheduler policy

- `src/multiplayer_turn_scheduler.h/.cpp` 是不可复制/移动的单一权威纯 simulation policy：最多四名参与者，turn 开始时复制并校验 roster，
  turn 内 snapshot 不受外部 vector 或 mid-turn join 影响；participant 以稳定 `player_id` 标识，generation 只作经
  transition 校验的 session metadata。
- roster 使用 canonical `player_id` 排序；每个完成 turn 记录其首位，下一 turn 从该 ID 的字典序后继开始，末尾
  回绕。因此 roster 增删不会把公平策略退化为固定 host-first。ordering key 包含 shared turn、round、slot 和
  `player_id`，current slot 在没有 accepted action 时保持稳定。
- action result 采用 `accepted_remains_eligible`、`accepted_finished`、`rejected`、`duplicate` 四种 typed
  disposition；后两者明确不推进 cursor/round。resume 要求精确旧 generation 且只允许 `+1`，并拒绝 overflow。
- disconnect 是 barrier-local 状态，不替代 authoritative session owner。只有 current `disconnected_grace` slot 可
  应用 timeout；`automatic_wait`/`remove_from_barrier` 先进入 `automatic_wait_pending`，调用方执行真实规则后才可
  调用 `record_automatic_wait_executed()`。removed participant 在本 turn snapshot 中保留到 world 完成。
- 所有参与者 terminal 后进入 `world_ready`；`claim_world()` 只可成功一次并转换到 `world_processing`，返回执行
  当前 shared-turn world phase 的 permission marker；`record_world_completed()` 拒绝未 claim、stale 和 double
  record。
- `tests/multiplayer_scheduler_test.cpp` 覆盖 invalid/duplicate roster、mid-turn join 延后、round-robin、
  rejected/duplicate 不推进、roster churn 首位轮转、四玩家无饥饿、generation resume、timeout/remove 以及真实
  alpha/beta registry/guard wait-only adapter。该 adapter 在 policy record 前实际调用 shared wait executor，并用
  scoped restore 保持临时 safe-mode 改动不泄漏。
- API 名称刻意使用 `record_automatic_wait_executed()`/`record_world_completed()`：纯 policy 只能记录编排器报告，
  无法证明外部 wait/world 副作用真的执行。production 必须提供不可绕过的 forced/scoped adapter。world ticket 也
  只约束 claim/record 状态，不证明真实 callback exactly-once；claim 后 callback 失败目前没有 retry、abort、
  rollback 或恢复协议。
- source audit 的详细 mixed-ownership 清单已写入 `DO_TURN_PHASE_AUDIT.md`：现有 moves 在 `player_end` 的
  `u.process_turn()` 补充；single-`u` walk/field/scent/monster/NPC/group-center 等仍需逐项适配，不能由纯 scheduler
  隐式解决。

### 生产 server/client transport 与客户端协议状态机

- `multiplayer_client_transport` 复用 standalone Asio/TCP frame contract，提供 async DNS/IPv4/IPv6 resolve、connect/read/write、1 MiB frame cap、有界 inbound/outbound/pending-write 队列和安全 stop/join。
- 客户端入站 frame 只可占用 `inbound_event_count - 1` 个槽，给 disconnect/protocol/transport terminal event 留出控制槽；饱和时 fail closed，而不是静默丢失关键状态。
- 服务端为每个 admitted lifecycle 预留 connected 与 terminal control capacity；socket 关闭后逻辑 connection slot
  仍保持占用，直到 simulation thread 从 inbound queue 消费该连接的 disconnect/protocol/transport terminal
  event。这样 connect/reset churn 不能复用刚关闭的 slot 并把更早 terminal event 挤出有界队列；terminal event
  消费后才释放 admission capacity。若按该不变量本不应发生的 connected/terminal control enqueue 仍失败，
  transport 会先持久记录全局 fatal detail，再停止 accept/I/O；`poll_event()` 与 dedicated-server error path 均可
  取得该 detail，不依赖已经饱和的普通 inbound queue。
- `multiplayer_client` 实现 hello/auth/resume、session/identity/generation 校验、scene/revision tracking、ping/pong、resync、pending command cache 和原 payload replay；首次握手失败可重新 fresh auth，resume-expired 会清除旧 session 后允许 fresh retry。
- resume 的 `last_client_sequence` 不越过最早未确认 command；晚到 pong 不能确认前面的 command。缓存 command result 可以早于更新后的 resume full scene，但不能早于该 command 自己的 `base_revision`。
- 服务端以同一个 256-command window 约束执行结果缓存和 resume replay：只允许重放仍保留的
  `PlayerCommand` sequence；过旧 floor 会令 session 过期，先前的 ping、sequence gap、已淘汰 command、解析失败
  或 application queue 拒绝的 sequence 都不能在 resume 后跨类型复用为新 command。客户端提交的未来 floor 只会
  抬高 application high-water，不会重新开放旧 sequence。
- command result 会抬高客户端最小 scene revision；在对应 scene 到达前客户端不重新进入 ready，避免下一命令基于旧画面 revision。
- 本地 payload 构建或 enqueue 失败不会消耗 sequence，避免下一条消息在同一 TCP session 中制造业务序列缺口。
- typed `DisconnectNotice` 严格验证 rejection enum、UTF-8/control characters 和 512-byte message 上限；客户端等待 pending command 归零后请求 release，并要求 ACK session/sequence 精确匹配。服务端先把 leave request 放进 simulation FIFO，在所有更早 command/result/scene 排队后进入 releasing，并用一个原子的 transport 操作排入 ACK 与 ordered close。resume record 只在 ACK bytes 已写完且 transport 报告精确 ordered-close completion 后删除；peer close、queue/read/write failure 或任何非精确关闭都会保留 resume 能力。
- rejected auth/resume 允许空 session，accepted response 必须有非空 session。
- full semantic scene payload 增加 512 KiB 移动端预算；超限 snapshot 从最外 Chebyshev radius 向内裁剪并保留 center/player anchors，tile/entity 的每轴相对距离限制为 128，仍受 1 MiB transport cap 保护。

### 桌面 connection/input/rendering 边界

- `main.cpp` 新增 `--connect`、`--connect-token-file`、可重复 `--connect-mod` 和 `--allow-insecure-client-lan`。非 loopback plaintext 默认拒绝；token 必须来自 private regular file，POSIX mode 为 `0600` 或更严格。
- 客户端按服务器相同的 core + ordered mods 计算 SHA-256 content manifest。definition-only 临时 `WORLD` 只用于匹配 ordered mod-interaction loading，退出时通过 `worldfactory::set_active_world()` 恢复 options/world 指针；不创建 save、不加载服务器世界、不运行客户端权威模拟。
- `run_multiplayer_client_ui()` 使用本地 `input_context` 解析方向键、Windows keybinding 和 Android touch shortcut；只发送 typed wait/move command。当前 UI 保持一次一个未确认动作，不做客户端移动预测。
- UI 每 30 秒发送 ping，120 秒无 pong 时把 half-open transport 标记为断线；已经认证但等待 command 所需 scene revision 的状态也继续 heartbeat。恢复仍由本地 confirm 显式触发 resume/fresh retry，不包含自动 backoff。
- curses fallback 从本地 terrain/furniture/trap/monster definitions 绘制 semantic scene。窗口尺寸变化时重建 scene/status windows，退出时恢复原 `g->w_terrain`。
- SDL tiles 路径通过 `cata_tiles::draw_remote_scene()` 使用本地 tileset 绘制 terrain、furniture、visible trap、monster 和 player appearance；renderer 不查询客户端 map、monster tracker 或 save state。
- `tools/multiplayer/network_client_ui_smoke.py` 通过 PTY 和断线 TCP proxy 驱动真实 `cataclysm --connect`，已接入 transport workflow，验证 local wait/move/reconnect/quit 与 exactly-once replay。

### Android launcher 与私有凭据

- Splash screen 现在允许选择 single-player 或 multiplayer，并提供 endpoint、64 位小写十六进制 token 和显式 insecure-LAN 例外输入。
- endpoint 保存在 app preferences；insecure-LAN consent 每次启动都恢复为未选中。token 只可在保存时绑定的完全相同 endpoint 上复用，换地址必须重新输入。
- token 写入 `getNoBackupFilesDir()/multiplayer/client-token.txt`，经临时文件、flush/fsync、`chmod` owner-only、原子 rename、父目录 fsync 和 app-private canonical path 检查后替换。
- `CataclysmDDA.getArguments()` 只从应用内 Intent extras 生成 `--connect`、`--connect-token-file` 和可选 LAN 参数；token 值不进入 argv 或日志。
- launcher 对 incomplete multiplayer Intent 生成明确无效的 `--connect` 参数，让 native command validator fail closed，且不会回退启动本地游戏；SDL Activity 启动后不再重定向到 mode chooser。已有 crash/launch/connect dialog 不会在 Activity resume 时叠加，运行中的 native argv 也不会被 `onNewIntent()` 静默替换。
- Android CMake source glob 使用 `CONFIGURE_DEPENDS`，新增 `src/multiplayer_*.cpp` 会触发增量 reconfigure；arm64 与 x86_64 debug APK 都已实际编译新的 client、transport 和 SDL remote renderer。

### 额外加固与本地工具链诊断

- server token reader 先用 `symlink_status` 拒绝 symlink/非普通文件，且 POSIX mode/symlink 行为有回归测试。client reader 同样 fail closed，并额外执行 128-byte size cap 与 `0600` 检查；真实 PTY smoke 使用 private token file 覆盖其成功路径。
- 普通 release PTY 曾在 `initscr()` 报 `corrupted size vs. prev_size`。SIGABRT backtrace 证明本地无 root 工具链错误地把系统 shared `libncursesw.so.6` 与 prefix static `libtinfo.a` 混链，和 multiplayer client 无关。补齐 prefix 的 runtime library 后普通 release UI smoke 通过。
- `check-multiplayer-build-env.sh linux` 现编译一个同时引用 `initscr()`/`tparm()` 的 probe，通过 `ldd` 要求 ncursesw/tinfo 来自同一动态 runtime root，并用 `nm` 拒绝 `_nc_doalloc`/`_nc_tparm_analyze` 被静态带入；已人工移除 runtime symlink 验证该门禁会按预期失败。

### Backend-neutral multiplayer build identity

- KVM Android 首次连接发现旧握手错误地复用了显示 `VERSION`：同源 SDL3 client 发送 `90e5fa3+SDL3`，
  headless server 发送 `90e5fa3`，因此在进入 authentication 前被拒绝。
- commit `e078eb6aef25a9cc72eea45793114c931a52b896` 增加独立 canonical multiplayer build ID：完整 40 位小写
  Git SHA，可选 `-dirty`。Make、CMake、Gradle、MSVC 对同一 source tree 生成同一值；UI backend、tiles/sound
  capability、generator tag 和显示 `VERSION` 不参与协议身份。
- build paths 拒绝非法格式；server/client 在 ID 为空或不可取得时拒绝 multiplayer startup。`--version` 单独打印
  该值，server structured log 记录它；baseline workflow 显式注入并验证 `${github.sha}` 出现在 Linux、Windows
  和 Android artifact 中。
- 只读审计发现初版 CMake target 可能因已有 `version.h` 跳过重算且未传递 configured override；Make 把
  `git diff --quiet HEAD` 的任何非零结果都转成 `-dirty`；Windows workflow 使用默认大小写不敏感的
  `-notcontains`。实际 `e078eb6` Windows artifact 文本是小写 canonical ID，但旧断言本身不保证大小写。
- commit `86336ea847bea45f727fd97d74a811a32712518c` 把 CMake generation 改为 always-run、content-aware target 并
  传递 override；Make 只接受 rc 0/1 为 clean/dirty，rc > 1 fail closed；Windows gate 改用 `-cnotcontains`。
- ADR-0004 与 refactor plan 第 13 节已记录该 durable compatibility rule，避免以后把 display branding 再次混入
  handshake identity。

## 验证证据

### Phase 3 phase adapter/source seams：Tier 1 Linux（当前 source state）

```bash
source build-scripts/activate-multiplayer-build-env.sh
./build-scripts/check-multiplayer-build-env.sh linux
make -j"$(nproc)" \
  COMPILER=g++-13 TILES=0 SOUND=0 RELEASE=1 LOCALIZE=0 \
  BACKTRACE=0 PCH=0 ASTYLE=0 tests
./tests/cata_test '[multiplayer][phase_adapter]' \
  --rng-seed 0 --user-dir /tmp/cdda-mp-phase-adapter-final
./tests/cata_test \
  '[multiplayer][turn_phase],[multiplayer][command_executor],[multiplayer][scheduler]' \
  --rng-seed 0 --user-dir /tmp/cdda-mp-phase-focused-final
./tests/cata_test '[multiplayer]' \
  --rng-seed 0 --user-dir /tmp/cdda-mp-phase-full-final

make -j"$(nproc)" AUTO_BUILD_PREFIX=1 \
  COMPILER=g++-13 RELEASE=1 LOCALIZE=0 BACKTRACE=0 PCH=0 \
  SANITIZE=address,undefined \
  WARNINGS='-Wall -Wextra -Wno-error=array-bounds' tests
ASAN_OPTIONS='detect_leaks=1:detect_stack_use_after_return=1:halt_on_error=1:abort_on_error=1' \
UBSAN_OPTIONS='print_stacktrace=1:halt_on_error=1' \
./tests/release-local-back-sanitize-cata_test \
  '[multiplayer][phase_adapter],[multiplayer][scheduler],[multiplayer][command_executor],[multiplayer][turn_phase]' \
  --rng-seed 0 --user-dir /tmp/cdda-mp-phase-adapter-sanitize-final
```

- Linux environment gate 通过；GCC 13 release `tests` target 完整构建成功。
- focused phase adapter：11 cases / 246 assertions，全过；新增故障注入在真实 wait 后故意提前推进 scheduler，使
  adapter 的正式 record 失败，验证 post-side-effect fault、world claim 拒绝和 replacement adapter 不可重试。
- focused turn-phase/command-executor/scheduler：12 cases / 447 assertions，全过。
- 完整 `[multiplayer]`：70 cases；68 通过，两个 full-avatar move-swap `[!mayfail]` 负面对照保持 expected failure；
  3,714 assertions 中 3,711 通过、3 个 expected failures，exit 0。
- sanitizer target 增量重建成功；定向 filter 为 21 cases / 670 assertions，全过，运行 314.022 秒；无 ASan、
  UBSan、LSan 或 stack-use-after-return finding。仅出现既有 GCC 13 initializer-list `array-bounds` optimizer warning，
  本 sanitizer build 继续用精确 `-Wno-error=array-bounds` 降级，不隐藏其他 warning。
- AStyle 3.1 `make ... astyle-check` 报 `no astyle regressions`；最终 `git diff --check` 通过。
- 这是 Tier 1 backend-neutral source evidence。Windows/Android production builds 未运行；adapter 未接 production
  runtime，所以 PTY loopback 未运行。二者均为按策略未运行，不是 blocker。

### Linux-first workflow/selector 本地门禁（当前 source state）

- actionlint 1.7.12 对 `.github/workflows/multiplayer-baseline.yml` 和
  `.github/workflows/multiplayer-transport-spike.yml` 均无输出、exit 0。
- PyYAML parse 成功；baseline 的 18 个、transport 的 15 个 Bash-compatible `run:` blocks 均通过 `bash -n`。
- transport selector 动态断言通过：manual `linux|windows|android|all` 精确选择目标；manual Linux/all 同时启用
  root-CMake contract；scheduler production-source commit `45be077` 只输出 Linux；实际修改 transport workflow 的
  commit `377feba` 输出 Linux+Windows+Android 并启用 root-CMake gate；包含 root CMake/version 变化的 `e078eb6`
  同样启用 root-CMake gate。
- baseline 与 transport selector 在 40 个零的 unresolved base 上均快速失败并提示 explicit manual target，不再
  隐式全矩阵。
- 静态 path mapping 断言通过：baseline 自动列表不再包含 root CMake/version、mixed `main.cpp`、routine
  transport/crypto `.cpp` 或 server config/log wildcard；`Makefile` 只选 Linux；public transport/crypto headers 和
  protocol/schema/generated header 在 push/PR 两份列表中选择 W/A actual-source package。transport 已移除纯 README
  与 Windows-only common props，manual default 为 Linux，standalone W/A 自动 selector 只匹配其 workflow/spike path。
- 本地 root CMake 定向命令以 GCC 13、curses、无 tiles/sound/localize/tests 配置成功，并构建 `get_version` target；
  本机因 user-prefix zlib/ncurses 额外传入 `CMAKE_PREFIX_PATH`，hosted Ubuntu job使用已安装系统开发包。
- 这是 selector/source lint evidence，尚不是修改后 workflow 的 hosted evidence。由于本批修改 workflow 自身，首次
  推送会按 cross-platform CI boundary 跑必要的 selector/platform jobs；后续 routine source push 才收敛为 Linux-only。

### Phase 3 scheduler policy 本地验证（上一 source state）

```bash
source build-scripts/activate-multiplayer-build-env.sh
make -j"$(nproc)" \
  COMPILER=g++-13 TILES=0 SOUND=0 RELEASE=1 LOCALIZE=0 \
  BACKTRACE=0 PCH=0 ASTYLE=0 tests
./tests/cata_test '[multiplayer][scheduler]' --rng-seed 0 \
  --user-dir /tmp/cdda-mp-scheduler-final-2
./tests/cata_test '[multiplayer][turn_phase]' --rng-seed 0 \
  --user-dir /tmp/cdda-mp-turn-phase
./tests/cata_test '[multiplayer][player_bridge]' --rng-seed 0 \
  --user-dir /tmp/cdda-mp-player-bridge
./tests/cata_test '[multiplayer]' --rng-seed 0 \
  --user-dir /tmp/cdda-mp-full-final-2

make -j"$(nproc)" AUTO_BUILD_PREFIX=1 \
  COMPILER=g++-13 RELEASE=1 LOCALIZE=0 BACKTRACE=0 PCH=0 \
  SANITIZE=address,undefined \
  WARNINGS='-Wall -Wextra -Wno-error=array-bounds' tests
ASAN_OPTIONS='detect_leaks=1:detect_stack_use_after_return=1:halt_on_error=1:abort_on_error=1' \
UBSAN_OPTIONS='print_stacktrace=1:halt_on_error=1' \
./tests/release-local-back-sanitize-cata_test '[multiplayer][scheduler]' \
  --rng-seed 0 --user-dir /tmp/cdda-mp-scheduler-sanitize-final-2

make ASTYLE_BINARY="$HOME/.cache/cdda-tools/astyle-3.1-3build1/root/usr/bin/astyle" astyle-check
git diff --check
```

- GCC 13 release `tests` target 已是最新并成功。
- focused scheduler：6 cases / 424 assertions，全部通过。
- focused turn phase：1 case / 8 assertions，全部通过。
- focused player bridge：5 cases / 325 assertions；三个稳定 registry/guard 正向 cases 的 322 assertions 通过，
  两个 full-avatar move-swap `[!mayfail]` 负面对照保留 3 个 expected failures，进程 exit 0。
- 完整 `[multiplayer]`：60 cases，其中 58 通过，两个 full-avatar move-swap cases 按既有 `[!mayfail]` 设计报告
  expected failure；3,496 assertions 中 3,493 通过、3 个 expected failures，进程 exit 0。
- 最新 sanitizer binary 重新构建后，focused `[multiplayer][scheduler]` 为 6 cases / 424 assertions 全过，exit 0；
  无 ASan、UBSan、LSan 或 stack-use-after-return finding。完整 `[multiplayer]` sanitizer suite 本批尚未重跑。
- AStyle 3.1 报告 `no astyle regressions`；`git diff --check` 通过。
- scheduler source 的 transport/protocol hosted run 已全绿；`45be077` baseline 的 Linux、Android 和 resources
  已成功。Windows validator failure 已由 `c9b2808` 的 hosted Windows package 定向成功取代。按当前策略无需为
  这个 Windows-only fix 等待 Android 全包重跑；Phase 3 exit gate 仍保持未关闭。

### Phase 3 scheduler source 与 Windows validator hosted evidence

- source commit：`45be077ac2d0d042190e54d1bdb77d48676b67e6`。
- transport/protocol run
  [`29340055386`](https://github.com/wsdx233/Cataclysm-DDA-Multiplayer/actions/runs/29340055386) 为 terminal
  `success`：Linux GCC 13/Clang 18 job 通过 production `[multiplayer]` tests、headless process smoke 和
  network-client UI process smoke；Windows x64 MSVC loopback 与 Android NDK arm64 compile jobs 也成功，但只覆盖
  standalone transport spike。
- baseline run
  [`29340056602`](https://github.com/wsdx233/Cataclysm-DDA-Multiplayer/actions/runs/29340056602) 为 terminal
  `failure`。translations、tileset、shaders、soundpack、Linux curses package 和 Android package jobs 均成功；
  唯一失败是 Windows x64 MSVC tiles+sound package job
  [`87109602068`](https://github.com/wsdx233/Cataclysm-DDA-Multiplayer/actions/runs/29340056602/job/87109602068)
  的 `Build package` step。
- 失败来自 `msvc-full-features/prebuild.cmd` 的手写 canonical build-ID validator 误拒合法 40 位小写 SHA；它发生在
  production compile 前，不能描述为 scheduler compile failure，也不能用 standalone transport Windows job冒充
  scheduler compile evidence。后续 `c9b2808` 完整 MSVC package job实际编译并关闭 Windows gate。
- 修复 commit `c9b28086e973fa497d5bd9f9a37e64a9ac22e464` 改为 PowerShell `-cmatch` 的 anchored、case-sensitive
  regex，并在 baseline workflow 的 package step 中先显式运行 prebuild，再分别对 prebuild 与 MSBuild 的非零
  exit code 立即失败。
- baseline run
  [`29344937410`](https://github.com/wsdx233/Cataclysm-DDA-Multiplayer/actions/runs/29344937410) 整体最终为
  `cancelled`，但 attempt 1 的 Windows x64 MSVC tiles+sound package job
  [`87126475702`](https://github.com/wsdx233/Cataclysm-DDA-Multiplayer/actions/runs/29344937410/job/87126475702)
  已完整 terminal `success`，覆盖 prebuild、MSBuild、windist、native version/help 与 package smoke。Linux package
  和 translations/tileset/shaders/soundpack jobs 同样成功。
- attempt 1 的 Android job
  [`87126475664`](https://github.com/wsdx233/Cataclysm-DDA-Multiplayer/actions/runs/29344937410/job/87126475664)
  在 arm64 release build 完成后，于 x86_64 debug build 因 hosted runner `No space left on device` 失败；attempt 2
  的 Android-only job
  [`87137475194`](https://github.com/wsdx233/Cataclysm-DDA-Multiplayer/actions/runs/29344937410/job/87137475194)
  随后由用户按新验证策略取消。run 整体不能标为全平台绿色，但这不是
  Android 或 scheduler 代码失败；`c9b2808` 没有修改 Android boundary，Windows-specific Tier 2 gate 已由成功 job
  关闭。Phase 3 shared-scheduler/exit gate 仍远未完成，`players.max` 继续为 `1`。

### Windows build-ID validator 修复的本地静态门禁

```bash
source build-scripts/activate-multiplayer-build-env.sh
./build-scripts/check-multiplayer-build-env.sh all
$HOME/.cache/cdda-tools/actionlint-1.7.12/actionlint \
  .github/workflows/multiplayer-baseline.yml
python3 - <<'PY'
import re
pattern = re.compile(r'\A[0-9a-f]{40}(?:-dirty)?\Z')
valid = [
    '45be077ac2d0d042190e54d1bdb77d48676b67e6',
    '45be077ac2d0d042190e54d1bdb77d48676b67e6-dirty',
]
invalid = [
    '45BE077AC2D0D042190E54D1BDB77D48676B67E6',
    '45be077',
    '45be077ac2d0d042190e54d1bdb77d48676b67e6-dirty-extra',
    'g5be077ac2d0d042190e54d1bdb77d48676b67e6',
]
assert all(pattern.fullmatch(value) for value in valid)
assert not any(pattern.fullmatch(value) for value in invalid)
PY
git diff --check
```

- Linux/Android environment gate 通过；该变更没有修改两者 toolchain。
- `actionlint` 无输出并返回 0；`git diff --check` 通过。
- anchored regex contract 的合法 40 位小写 SHA、合法 `-dirty`、大写、短值、非法后缀正反例检查通过。
- 当前机器没有 Windows shell；上述静态检查不能替代平台证据。最终 Windows 证据已由 run `29344937410`
  attempt 1 的成功 MSVC package/windist/native smoke job 提供。

### Hosted Phase 1/baseline（已绿色）

- baseline run [`29219328448`](https://github.com/wsdx233/Cataclysm-DDA-Multiplayer/actions/runs/29219328448)，提交 `cd18703`，结论 `success`：
  - Linux curses artifact `8267944561`，digest `b4b2083d87190f8efada2c2d4e49e28370efc2b19f9bca0d2a31c010124e7dde`；
  - Windows x64 MSVC tiles+sound artifact `8267993011`，digest `cd291572d18ae2cf76eedbd07dc226d4ec211f51237e88f3044bc896fc21824f`；
  - Android arm64 release APK artifact `8267899979`，digest `981a9a6d3b8f6c4aa38f22dbbcfea1b9c76dbe1713eac0489049f222939a3e81`。
- transport/protocol run [`29219953446`](https://github.com/wsdx233/Cataclysm-DDA-Multiplayer/actions/runs/29219953446)，提交 `bc7efe0`，结论 `success`：
  - Linux GCC 13/Clang 18 + production game/process artifact `8268340815`，digest `ff04b8ecb4536e6e2bcc5a0aceaf7c04df8706c0aacf8dcda4f7a35998017d6e`；
  - Windows MSVC artifact `8267835930`，digest `e509b6579b874ce57cfbabda488cbff24a48c2757a0ba19a6d3f286a75eaaf78`；
  - Android NDK arm64 artifact `8267835710`，digest `1ea5da10ed8e951201d2cae95d58850b4edc982a69f52172ca2c0990ffb6ad5c`。

这些 runs 关闭 Phase 1 hosted gate，但早于本批 graphical/network client source；本批自己的 hosted 证据记录如下。

### Hosted Phase 2 network-client batch（已绿色）

- baseline run [`29303564150`](https://github.com/wsdx233/Cataclysm-DDA-Multiplayer/actions/runs/29303564150)，
  source `6403a949fb537be14ec4d5757f295adbf5c5f99b`，结论 `success`；Linux job `18m57s`、Android
  job `39m04s`、Windows job `32m34s`：
  - Linux curses artifact `8299663785`，digest `d9e228af65de0dcf52c4849a63a4de76267c8b1701de560518b13c01e4648069`；
  - Windows x64 MSVC tiles+sound artifact `8299880246`，digest `1ca0a557fc27235bda25b2579542c4f058c8c554561612ff37c13018e9289b44`；
  - Android arm64 release artifact `8299976619`，digest `d3d7382affbd7342951ca32957be2521165fbf2548f74e1d68d6ca22f86b4413`；
  - Android x86_64 debug artifact `8299977114`，digest `351094bbbd92664bb1245b586db978a257aed05daef6a51ae4316b8e39b3b16f`；
  - tileset artifact `8299414855`，digest `6403077a11ae9696a41780f162553f980fcb594f1395b2d0f8daac9844eaadde`；
  - shaders artifact `8299426143`，digest `098ede9628ca24e16ac8df6253ca8b88c26ce86e7d57fcd5048d274d674e92ee`；
  - soundpack artifact `8299403377`，digest `37749d3ac9d82f8199a57f6ce603e0785bf3151062e95c9c22177a8921cacee4`；
  - translations artifact `8299405280`，digest `b7ad074c2acb2ff9ae0fc9d7fae68cbb063082ec12332353c5f63f2b0156650b`。
- transport/protocol run [`29303564152`](https://github.com/wsdx233/Cataclysm-DDA-Multiplayer/actions/runs/29303564152)，
  同一 source，结论 `success`；Linux job `36m05s`、Windows job `58s`、Android job `33s`：
  - Linux GCC 13/Clang 18 + production game/process artifact `8299903967`，digest `0b38eaf73e9178895b8fe589573919582bf0144f6f7fa7346469966a14061013`；
  - Windows MSVC artifact `8299413066`，digest `f203a9ab7fb6d59aa65fe5ea671ae0bb90621bc7c77e4d59c23d775da21ea430`；
  - Android NDK arm64 artifact `8299407650`，digest `2c58a77c426dadf310948c945e64e25331dea2c7313ef2e62ab7effe41b37552`。

这些 terminal-success runs 关闭本批 hosted platform build gate，但 Android jobs 仍只是 package/compile evidence，
没有 emulator/真机 runtime lifecycle 结论。

### Hosted canonical build-ID gate（`e078eb6` 已绿色）

- baseline run
  [`29328086046`](https://github.com/wsdx233/Cataclysm-DDA-Multiplayer/actions/runs/29328086046)，source
  `e078eb6aef25a9cc72eea45793114c931a52b896`，terminal `success`：
  - Linux curses artifact `8308833498`，digest `679fdda2d52fd71b9a63172d979494f5cb380998b6ee0a8fa385c372d0a68d9b`；
  - Windows x64 MSVC tiles+sound artifact `8309536169`，digest `109c084bf44b8177ba9c5094402798d8c28af014fc149afc3d78c5c835da342b`；
  - Android arm64 release artifact `8309741656`，digest `c6cc827c9f467a65e108efb19b43d50cb6a4649632328daec266396da4ea3697`；
  - Android x86_64 debug artifact `8309743121`，digest `16c24f0425f7fe2490b0c1ca90a9feee818d9823225137520b270b84549e6c12`。
- artifact names 带完整 canonical SHA；Linux/Windows `--version` 和 Android 两个 `libmain.so` 均含同一小写 ID。
  Windows executable 同时报告 `+tiles, +sound`。当时 workflow 的 `-notcontains` 比较不区分大小写；实际 artifact
  文本为小写，但只有 `86336ea` 的新 `-cnotcontains` gate 能证明 assertion 本身区分大小写。
- transport/protocol run
  [`29328086326`](https://github.com/wsdx233/Cataclysm-DDA-Multiplayer/actions/runs/29328086326)，同一 source，
  terminal `success`：
  - Linux GCC 13/Clang 18 + production game/process artifact `8309668402`，digest `c020ceba6cae82c599599ba23a0e309af05b928af194cfa5cd0f998140522fff`；
  - Windows MSVC artifact `8308785601`，digest `7ff2b126611a3f3325f2ffe97d5b7d6dd13e7d843e52b1ca81118d515995bc5c`；
  - Android NDK arm64 artifact `8308782103`，digest `64f0e150407c73d046f5fec178fd1764c449570afe925d2d7d400b68e8cdd769`。
- Linux job 通过 pinned FlatBuffers generation、vendored Asio comparison、GCC/Clang loopback、生产
  `[multiplayer]` suite、真实 headless command/resume smoke 和 local-input network-client UI resume smoke；
  Windows MSVC loopback 与 Android NDK arm64 compile 也通过。

这两条 runs 关闭 `e078eb6` 自身的 hosted gate。Android KVM runtime/lifecycle 证据仍精确属于 `e078eb6`，而不是
后续 `86336ea`。

### Hosted build-generator hardening gate（`86336ea` 已绿色）

- baseline run
  [`29330811779`](https://github.com/wsdx233/Cataclysm-DDA-Multiplayer/actions/runs/29330811779)，source
  `86336ea847bea45f727fd97d74a811a32712518c`，运行区间 `2026-07-14T11:59:04Z`–`12:45:09Z`，7 个 jobs
  全部 terminal `success`：
  - Android x86_64 artifact `8311028317`，size `286233515`，digest `1106f66e0d052e73cc9804f26fbc57f919f623b0fb793f152f204b345790a661`；
  - Android arm64 artifact `8311026327`，size `180771669`，digest `416606fdd8cb75f0573aa6e0ec1825d258696cfbb95fce0a0d7018b1be6cf7f8`；
  - Windows x64 MSVC artifact `8310637856`，size `314192240`，digest `3584f373bdd0087664126c47cb8baa98b6fbaab8112cafd9bbd949f2a96221a3`；
  - Linux curses artifact `8309916226`，size `183112210`，digest `9d012e8ca91b5142f44a9c514fe9a27849d64c1710b7ab8576b08210a160572a`；
  - shaders artifact `8309904857`，size `28669`，digest `9097d36d426b1a0059a47e822cf539509b91bb63135b418a6b5a408038f4bf6a`；
  - tileset artifact `8309890914`，size `4733394`，digest `bb638007a3f6d01821ccca8a79ac02e8de1d84124e9662d46429dd07797b373b`；
  - translations artifact `8309873347`，size `85605345`，digest `aa132c98c3cfae2b40e9d958c11ec8e1ed82a7c1a2f014a73bbafb511c5cbd12`；
  - soundpack artifact `8309872172`，size `137709159`，digest `2c75644dc5b984aac604f0de508045d3f555ec820b2005a56ec9d39ccc0e4504`。
- Windows native version smoke 通过 `-cnotcontains` 大小写敏感 canonical-ID gate；Linux Make、Windows MSVC 与
  Android Gradle package paths 的 artifact checks 均确认精确小写 full SHA。顶层 `src/CMakeLists.txt` 的
  always-run/override path 不由这些 package jobs 直接调用，其证据来自下文的本地 Ninja/Unix Makefiles 回归。
- transport/protocol run
  [`29330811746`](https://github.com/wsdx233/Cataclysm-DDA-Multiplayer/actions/runs/29330811746)，同一 source，
  运行区间 `2026-07-14T11:59:04Z`–`12:37:29Z`，3 个 jobs 全部 terminal `success`：
  - Linux GCC 13/Clang 18 + production game/process artifact `8310820503`，size `147781`，digest `8422a7f7bcfd4ddea02145e287f267bfa1ba44f7d365d99d795c664f9ec0b364`；
  - Windows MSVC artifact `8309890710`，size `54055`，digest `c03af2a211ab5221cf38ab4271a774e67e8464da343fa1e636284858e4a67ad7`；
  - Android NDK arm64 artifact `8309880601`，size `1705305`，digest `c10815e1a2fbfda78531e453c7f23a50365b7aef80d3b498889d2d0e46f2c074`。
- transport run 重跑 pinned FlatBuffers/Asio、GCC/Clang/MSVC loopback、生产 `[multiplayer]` tests、真实 headless
  command/resume smoke、local-input network-client UI resume smoke 与 Android NDK compile，全部通过。

该最终 gate 与 `e078eb6` 的 Android KVM lifecycle 共同关闭 Phase 2；关闭日期为 2026-07-14。

### GCC 13 release

```bash
source build-scripts/activate-multiplayer-build-env.sh
make -j8 \
  COMPILER=g++-13 RELEASE=1 LOCALIZE=0 BACKTRACE=0 PCH=0 \
  cataclysm tests
./tests/cata_test '[multiplayer]' \
  --rng-seed 0 --user-dir /tmp/cdda-mp-final-client-suite-terminal-fix
```

结果：构建成功；53 test cases，其中 51 通过、2 个含完整 avatar move-swap reference-identity 对照的 cases 按既有 `[!mayfail]` 设计报告预期失败；3,066 assertions 中 3,063 通过、3 个为预期 `!mayfail`。新增覆盖包括 3,721 个长 ID tile 的 scene-budget 裁剪、fresh retry/expired resume、command-result scene floor、pending-scene heartbeat、256-command replay floor/cross-type reuse 拒绝、graceful ACK sequence、reserved control capacity、command → leave FIFO、result/scene → ACK write-drain/ordered socket close、所有失败路径的 resume 保留，以及 `maximum_connections=1`/`inbound_event_count=2` 下 closed logical slot 在 terminal event 被消费前拒绝 reset churn、消费后才允许 replacement connection 的回归。focused server_lobby/dedicated_server/transport 子集为 17 cases / 1,686 assertions，client 子集为 12 cases / 711 assertions，均全部通过。

### ASan/UBSan/LSan

```bash
make -j8 AUTO_BUILD_PREFIX=1 \
  COMPILER=g++-13 RELEASE=1 LOCALIZE=0 BACKTRACE=0 PCH=0 \
  SANITIZE=address,undefined \
  WARNINGS='-Wall -Wextra -Wno-error=array-bounds' \
  tests release-local-back-sanitize-cataclysm
ASAN_OPTIONS='detect_leaks=1:detect_stack_use_after_return=1:halt_on_error=1:abort_on_error=1' \
UBSAN_OPTIONS='print_stacktrace=1:halt_on_error=1' \
./tests/release-local-back-sanitize-cata_test '[multiplayer]' \
  --rng-seed 0 --user-dir /tmp/cdda-mp-final-client-sanitize
```

最终 source 的 sanitizer rebuild 成功；53 cases，其中 51 通过、2 个含既有完整 avatar move-swap 对照的 cases 按预期报告失败；3,066 assertions 中 3,063 通过、3 个为预期 `!mayfail`。无 ASan、UBSan、LSan 或 stack-use-after-return finding。诊断构建仍只对既有 GCC 13 initializer-list `array-bounds` optimizer warning 降级 `-Werror`，不是最终 warning policy 的变化。

### 真实 network-client UI / resume

最终 source state 的普通 release 与 ASan/UBSan binary 都通过 `tools/multiplayer/network_client_ui_smoke.py`：

```bash
python3 tools/multiplayer/network_client_ui_smoke.py \
  --client "$PWD/cataclysm" \
  --backend-port 38265 \
  --token-file /tmp/cdda-mp-ui-smoke-final-release-terminal-fix/server-auth-token.txt \
  --user-dir /tmp/cdda-mp-ui-smoke-final-release-terminal-fix/client-user \
  --transcript /tmp/cdda-mp-ui-smoke-final-release-terminal-fix/client.transcript \
  --event-log /tmp/cdda-mp-ui-smoke-final-release-terminal-fix/client-events.txt

ASAN_OPTIONS='detect_leaks=0:detect_stack_use_after_return=1:halt_on_error=1:abort_on_error=1' \
UBSAN_OPTIONS='print_stacktrace=1:halt_on_error=1' \
python3 tools/multiplayer/network_client_ui_smoke.py \
  --client "$PWD/release-local-back-sanitize-cataclysm" \
  --backend-port 37263 \
  --token-file /tmp/cdda-mp-ui-smoke-final-sanitize/server-auth-token.txt \
  --user-dir /tmp/cdda-mp-ui-smoke-final-sanitize/client-user \
  --transcript /tmp/cdda-mp-ui-smoke-final-sanitize/client.transcript \
  --event-log /tmp/cdda-mp-ui-smoke-final-sanitize/client-events.txt \
  --timeout-seconds 180
```

两个 matching server fixtures 分别由同一个 release/sanitizer binary 在上述 `/tmp` 根目录中以
`--init-server-config` 后接 `--server` 启动；实际 loopback backend ports 已按最终 `server.json` 记录在命令中。

1. 真实 dedicated server 完成 hello/auth 和初始 visibility-filtered scene；
2. PTY 本地 `.` 解析为 semantic wait；proxy 在命令 bytes 已转发后断开；
3. 本地 confirm 发起 resume，服务器返回新 session generation 和 full scene；
4. 客户端原 payload 重放得到 duplicate result，不重复执行 wait；
5. PTY 本地 `l` 解析为 east move，收到 command result 与新 scene；
6. PTY 本地 quit 等待 pending command settlement，server 按 simulation FIFO 排入 typed release ACK，在 ACK write drain 与 ordered socket close 完成后删除 resume record，客户端随后干净退出；最后 server SIGTERM 保存并 shutdown。

release 与 sanitizer 的 command statuses 都精确为 `accepted (0), duplicate (2), accepted (0)`，两次客户端均返回 0，transcript 均为 7,503 bytes；events 都精确为 `listening`、`player_authenticated`、`command_result`、`player_disconnected`、`player_resumed`、`command_result`、`command_result`、`player_disconnected`、`save_completed`、`shutdown`。sanitizer transcript 不含 ASan/UBSan/LSan marker。第一次 sanitizer 尝试只超过旧的 180 秒 server-startup allowance，并非 sanitizer finding；fresh-root 重试把 sanitizer server startup allowance 放宽到 600 秒，客户端交互的 `--timeout-seconds 180` 保持不变，随后完整通过。sanitizer process smoke 使用 `detect_leaks=0`，因为 ncurses 本身在进程退出时有已知约 1.9 KiB library leak。server stdout/stderr 每个非空行均为无 ANSI 的合法 JSON。

### `e078eb6` canonical build identity 本地验证

```bash
source build-scripts/activate-multiplayer-build-env.sh
./build-scripts/check-multiplayer-build-env.sh all
./cataclysm --version
./tests/cata_test '[multiplayer][protocol]' \
  --rng-seed 0 --user-dir /tmp/cdda-multiplayer-e078-test-user
./tests/cata_test '[multiplayer]' \
  --rng-seed 0 --user-dir /tmp/cdda-multiplayer-e078-test-user
PATH="$HOME/.cache/cdda-tools/actionlint-1.7.12:$PATH" \
  "$HOME/.cache/cdda-tools/actionlint-1.7.12/actionlint" \
  .github/workflows/multiplayer-baseline.yml \
  .github/workflows/multiplayer-transport-spike.yml
```

- environment gate 通过；clean-ID rebuild 后 `--version` 显示 UI `e078eb6`、multiplayer build ID
  `e078eb6aef25a9cc72eea45793114c931a52b896`、`-tiles, -sound`。
- focused `[multiplayer][protocol]`：8 cases / 240 assertions，全绿。
- 完整 `[multiplayer]`：process exit 0，52 cases / 3,069 assertions；只有
  `multiplayer_player_slot_test.cpp:1078/1112/1115` 的三个既有 full-avatar-swap reference-identity
  `[!mayfail]` 对照报告预期失败。
- Make explicit-valid/invalid/auto ID、direct CMake valid/no-rewrite/invalid、Gradle explicit/auto/invalid/no-Git
  检查均通过；MSVC generation/package 后由 baseline run `29328086046` 的 hosted `windows-2022` job 验证。
- actionlint 1.7.12 对两个修改后的 workflows 均通过。

### `86336ea` build-generator hardening 本地复核

激活 pinned environment 后执行：

```bash
make version MULTIPLAYER_BUILD_ID="$(git rev-parse HEAD)"
GIT_INDEX_FILE=/ make version
cmake --build /tmp/cdda-build-id-cmake --target get_version
```

- explicit Make override 写入精确 clean ID `86336ea847bea45f727fd97d74a811a32712518c`。
- 故意令 Git index probe 失败时，generator 报 dirty-state rc 128，Make 返回 rc 2，且既有 `version.h` SHA 不变；
  基础设施错误不再被误标成 `-dirty`。
- CMake auto target 写入 `86336ea847bea45f727fd97d74a811a32712518c-dirty`；间隔一秒重复执行后
  `version.h` mtime 不变，证明 target 每次核对 identity 但 content-aware write 不触发无意义重编。随后
  `version.h` 已用 clean `86336ea` override 恢复。
- commit 已推送；baseline run `29330811779` 与 transport/protocol run `29330811746` 随后均 terminal
  `success`，补齐 hosted Windows/package/transport evidence。

### Android arm64 / x86_64 debug APK

```bash
source build-scripts/activate-multiplayer-build-env.sh
cd android
./gradlew -Pj=8 \
  -Pabi_arm_32=false -Pabi_arm_64=true \
  -Pabi_x86_32=false -Pabi_x86_64=false \
  -Plocalize=false assembleExperimentalDebug
```

最终 source state 的 arm64 构建为 `BUILD SUCCESSFUL in 1m 21s`，39 tasks（13 executed、26 up-to-date），APK SHA-256 为 `0f6d7324675c4291281d072ba631ab7a2477d9507033eefb3102b9fec6e6da6b`，且只包含 `arm64-v8a/libmain.so`。

x86_64 使用同一命令但令 `-Pabi_arm_64=false -Pabi_x86_64=true`，结果为 `BUILD SUCCESSFUL in 1m 19s`，39 tasks（13 executed、26 up-to-date），APK SHA-256 为 `06e70f034bd9761aa98f6194798ffcc8706659ec387ebc590494340af1af66d1`，且只包含 `x86_64/libmain.so`。更早一次 x86_64 Java compile 因 Android API 没有 `OsConstants.O_DIRECTORY` 失败；改为以 `O_RDONLY` 打开 app-private 父目录再 fsync 后，连续 x86_64/arm64 构建均绿色。

两份 APK ZIP 都完整、声明 `android.permission.INTERNET`，并包含 `connectMultiplayer`、`launchModeTitle`、
`multiplayerConnectTitle`、endpoint/token validation、`multiplayerExistingSession`、LAN consent/security notice 和
token-storage resources。该证据证明 Java/C++/SDL tiles source integration，不是 emulator/真机连接证据。

### Canonical build ID 与 Android API 35 KVM runtime（本地绿色）

构建与 ABI 证据：

```bash
source build-scripts/activate-multiplayer-build-env.sh
cd android
./gradlew clean \
  -Pj="$(nproc)" \
  -Pabi_arm_32=false -Pabi_arm_64=false \
  -Pabi_x86_32=false -Pabi_x86_64=true \
  -Plocalize=false assembleExperimentalRelease
```

- x86_64 Release clean build 为 `BUILD SUCCESSFUL in 54s`。unsigned APK SHA-256：
  `e3b308a16bb3b93192d9f107eeac7e1db303b407724ef5922596fb0d999d3287`。
- 为 emulator 安装而使用 Android Debug certificate 生成的 diagnostic signed copy SHA-256：
  `238f5343c5dc87d34ba062bdc573bf4464cae91c25ece08da1287a50eb3679b8`。该签名只用于本地诊断，不是
  production signing 或 hosted evidence。
- APK 中 `x86_64/libmain.so` 包含精确 canonical ID
  `e078eb6aef25a9cc72eea45793114c931a52b896`。

加速与启动证据：

```bash
source build-scripts/activate-multiplayer-build-env.sh
sg kvm -c "\"$ANDROID_HOME/emulator/emulator\" -accel-check"
```

- 不修改系统权限即可通过 `sg kvm` 使用已有 group access；`-accel-check` 报告 KVM version 12 usable。同一 API
  35 x86_64 AVD cold boot log 为 `18.706s`。
- 从正常 exported `SplashScreen` 路径启动，而不是 root/direct Activity launch；11,523 个 assets 在 90 秒内升级
  完成，使用已保存 endpoint/token 与当次显式 LAN consent 进入 client，全程无 ANR。
- 本地 diagnostic package 缺少 `grayscale.frag.spv`，对应 shader variant 被禁用，但 ASCIITiles remote scene 在
  首个 command 前已渲染；hosted baseline 提供 shaders，因此这是本地 diagnostic limitation，不是 artifact
  contract failure。move 前后截图不同，证明画面随 scene revision 更新。

受控 server log 位于 `/tmp/cdda-mp-android-device.GnKAt1/server.stdout`，事件精确为：

1. server 以 canonical ID 监听；`2026-07-14T10:40:00.604Z` authentication 成功，player
   `f313d072-4ded-43c5-a36b-a8f72e24fe1a`、character `1`、generation 1。
2. 仅发送一次 wait：sequence 3、type 1、accepted、revision 2；随后仅发送一次 right move：sequence 5、type 2、
   accepted、revision 4。
3. HOME 后 native PID `4820` 保留，但 transport 断开 generation 1；existing Activity 以 HOT `92ms` 恢复，Enter
   使用同一 identity 恢复 generation 2；sequence 12 wait accepted、revision 6。
4. Android 内定向 `ss -K` 断开 generation 2；airplane mode enable/disable 后 route 在 3 秒内恢复，Enter 使用同一
   identity 恢复 generation 3；sequence 18 wait accepted、revision 8。
5. `Q` 断开 generation 3；server `SIGTERM` 记录 `save_completed` revision 9、220ms，然后 `shutdown`。

测试结束后已恢复 `AUTO_KEYBOARD=true`、`show_ime_with_hard_keyboard=1`、`policy_control`/
`hide_error_dialogs` null、airplane mode 0、AVD `hw.keyboard=no`，并停止 emulator。先前 `-accel off` 软件模拟的
ANR 与 accidental held-touch flood 仅保留为不计入门禁的诊断历史；它们不再是 active blocker。Android
auth/render/controlled wait+move/pause-resume reconnect/network reconnect runtime gate 现为本地绿色。

### Schema、依赖、格式和 workflow

```bash
source build-scripts/activate-multiplayer-build-env.sh
make ASTYLE_BINARY="$HOME/.cache/cdda-tools/astyle-3.1-3build1/root/usr/bin/astyle" astyle-check
python3 -m py_compile tools/multiplayer/network_client_ui_smoke.py
bash -n build-scripts/check-multiplayer-build-env.sh
PATH="$HOME/.cache/cdda-tools/shellcheck-0.9.0-1/root/usr/bin:$PATH" \
  "$HOME/.cache/cdda-tools/shellcheck-0.9.0-1/root/usr/bin/shellcheck" \
  build-scripts/check-multiplayer-build-env.sh
PATH="$HOME/.cache/cdda-tools/shellcheck-0.9.0-1/root/usr/bin:$PATH" \
  "$HOME/.cache/cdda-tools/actionlint-1.7.12/actionlint" \
  .github/workflows/multiplayer-baseline.yml \
  .github/workflows/multiplayer-transport-spike.yml
python3 - <<'PY'
import pathlib
import subprocess
import tempfile
import yaml

for path in [pathlib.Path('.github/workflows/multiplayer-baseline.yml'),
             pathlib.Path('.github/workflows/multiplayer-transport-spike.yml')]:
    data = yaml.safe_load(path.read_text())
    scripts = []
    for job_name, job in (data.get('jobs') or {}).items():
        for idx, step in enumerate(job.get('steps') or []):
            run = step.get('run') if isinstance(step, dict) else None
            shell = step.get('shell', '') if isinstance(step, dict) else ''
            if isinstance(run, str) and ('bash' in shell or not shell):
                scripts.append((job_name, idx, run))
    for job_name, idx, script in scripts:
        with tempfile.NamedTemporaryFile('w', suffix='.sh') as source:
            source.write(script)
            source.flush()
            result = subprocess.run(['bash', '-n', source.name], text=True,
                                    capture_output=True)
            if result.returncode:
                raise SystemExit(f'{path}:{job_name}:step {idx}: {result.stderr}')
    print(f'{path}: YAML parsed; {len(scripts)} Bash-compatible run blocks passed bash -n')
PY
./build-scripts/check-multiplayer-build-env.sh all
git diff --check
```

- FlatBuffers 1.12.0 archive SHA-256 已核对，`generate.sh --check` 通过；GCC 13 只使用 `-Wno-error=stringop-overflow` 处理固定第三方 optimizer false positive。
- Asio `asio-1-38-1` archive、license 和 vendored headers 逐字节一致。
- AStyle 3.1 `make astyle-check`：通过。
- ShellCheck 0.9.0：changed Bash 通过；actionlint 1.7.12：两个 workflow 通过。actionlint 首次发现两个仅作计数的
  `attempt` loop variable 为 SC2034，改为 `_` 后复检绿色；本地仍没有 PowerShell parser。`e078eb6` 的 hosted
  Windows package job 已通过，`86336ea` 新增的大小写敏感 assertion 也由 run `29330811779` 终态验证通过。
- workflow YAML parse：通过；baseline 17 个、transport 13 个 Bash-compatible `run:` blocks 经 `bash -n`：通过。
- `python3 -m py_compile tools/multiplayer/network_client_ui_smoke.py`、`bash -n build-scripts/check-multiplayer-build-env.sh`、`git diff --check`：通过。
- `./build-scripts/check-multiplayer-build-env.sh all`：通过；Linux ncursesw/tinfo runtime 与 Android pinned SDK/NDK/JDK 均绿色。

## 已知限制和未完成项

- Phase 3 scheduler commit `45be077` 的 transport/protocol gate 已全绿；其 baseline 的 Linux、Android 与
  resources 成功，Windows validator failure 已由 `c9b2808` 的 hosted MSVC package 定向成功关闭。run
  `29344937410` 整体因 Android runner no-space 后的 policy cancellation 为 `cancelled`，不能称为全平台绿色，
  但按当前 Tier 2 策略不构成 active blocker。
- Phase 3 scheduler 与当前仅由测试调用的 source phase adapter/seams 已存在；stable registry/runtime、active-player guard 和
  human tracker 基础已复用，但 adapter 没有 production caller，dedicated-server routing 与 authoritative
  session/runtime directory 尚未接入。`players.max > 1` 仍未实现。
- 五个 `do_turn()` trace labels 不是 ownership boundary。`player_begin`/`player_input`/`player_end` 仍混入 world/UI
  工作；当前 `do_turn_remote()` 仍让单 avatar 用尽 moves 才执行一次 world phase。moves 在 `player_end` 的
  `u.process_turn()` 补充；first-turn 回归已保持现有顺序，但普通稳态 turn 的完整多人 off-by-one 等价仍待 production
  phase ownership 拆分后验证。
- adapter 已证明真实 forced wait 的 execute-before-record、目标 bookkeeping、root restore 和 in-memory fail-stop，
  但 production 代码仍可绕过它。actual `process_legacy_single_player_bubble_turn()` 没有置于 world claim 后；world
  test 只是 lambda。fault 也没有 typed save/shutdown/restart recovery，不能宣称真实 bubble exactly-once。
- adapter 拒绝 offline runtime。真实 disconnect 必须分离 transport disconnected、barrier disconnected 和 runtime
  offline，在 forced wait 完成前保留 active stable owner；当前 `disconnect_multiplayer_player()` 的时序不能直接
  复用为 barrier timeout。它还拒绝断开 active root runtime，单玩家 server 必须先决定 neutral server context 或
  selected-root/lifecycle 解耦，才能在 forced wait 后合法进入 offline。
- lobby resume generation 与 `multiplayer_player_runtime::session_generation()` 仍可能分叉；scheduler participant key
  尚无统一 authoritative owner。当前 lobby 会先递增 generation、更新 resume record 并构造 accepted response，之后
  才 emit event；必须改成 pending auth/resume -> simulation directory commit -> complete response 两阶段。当前 resume
  request 不携带 generation，expected old generation 应由 server token record 解析，不应为此无意修改 wire schema。
  lobby/scheduler 的 `UINT64_MAX`、runtime/save 的 `INT64_MAX` 边界和 client“只要变大”检查也必须统一为
  `< INT64_MAX` 且 exact `+1`。
- 本批 phase-adapter shared source 只取得 Tier 1 Linux release/sanitizer evidence。Windows/Android production
  build 按策略未运行；transport standalone W/A probes 即使绿色也不构成本批 source evidence。
- `game::walk_move()` 仍使用固定 `game::u`；human-human collision、monster target/attack、death、field、single
  `typescent`/scent center、NPC/overmap anchor、tether/group-centered shift 和 player-state isolation 均未关闭。
- 本地 Android diagnostic package 缺 `grayscale.frag.spv`，因此该 shader variant 被禁用；ASCIITiles scene 已正常
  渲染，hosted baseline 会提供 shaders。先前 `-accel off` ANR/held-touch flood 是已被 KVM run 取代的非计数历史，
  不是 active blocker。
- remote scene 仍只有 full snapshot；512 KiB fitter 会有损缩小可见半径，尚无 delta/chunk/compression 或 reduced-viewport metadata。scene 已携带 lighting byte，但 curses/tiles renderer 暂按全亮绘制；isometric terrain/entity painter ordering 也未完成。items、fields、vehicles、overlays、messages、sound、avatar replica/panels 仍缺失。
- visibility regression 目前覆盖遮挡怪物不泄漏，但 ADR-0006 要求的隐藏陷阱、未探索地形、不可见物品以及 delta 路径 leak matrix 尚未完成；其中 items/delta 也尚未实现。
- heartbeat 目前只有 30 秒 ping、120 秒 timeout 与手动 confirm reconnect；Android 的最小 pause/resume、定向
  TCP reset 和 airplane route recovery 已绿色，但长时间 background timing、自动 retry/backoff、进程死亡恢复和
  graceful-disconnect timeout 仍无运行证据。若服务端在 clean release 时不回 ACK，UI 仍需第二次 quit 才能强制离开。
- durable process-restart resume 属于 Phase 4：resume token、pending command 和 scene revision 当前仍只在 native process 内存中，进程杀死后不能继续旧 session；后续需 app-private、版本化、原子 checkpoint，但不阻塞当前 Phase 3 scheduler/adapter source slice。
- clean `DisconnectNotice` 已验证 command settlement、ACK write drain、ordered close 以及仅在精确 completion 后清除 resume record；但多数 protocol/auth/application 错误仍通过 transport close reason 而非 typed disconnect payload 返回。
- client token reader 的 size/mode/symlink 检查仍存在 path-check → open 的 TOCTOU 窗口；Windows private-file ACL 尚未由本地平台证据验证。
- 服务器仍只有一个 `active_remote_session`，command cache 也只按 sequence 建索引；config 继续拒绝
  `players.max > 1`。当前 policy 没有接入 session ownership、command routing、scene/result routing 或真实 world
  execution。
- save 仍是 canonical single-avatar generation；没有 multi-player runtime split、RNG engine save、generation fallback 或 portable character。
- 当前动作只有 wait 和八方向平面 move。所有其他动作必须保持明确未支持，不能进入 server blocking UI。
- 无嵌入式 TLS；loopback 默认、trusted-LAN 显式例外和外部 authenticated tunnel 政策保持不变。

## 下一门禁和首个动作

Phase 2 已关闭；Phase 3 scheduler、phase adapter/source seams 和 two-runtime in-process wait-only test 已有 Tier 1
Linux evidence。下一代码门禁是 **authoritative production session/runtime directory**，不是立即启用第二 client 或
move。首个要检查的文件和命令是：

```bash
rg -n 'active_remote_session|session_generation|resume|disconnect' \
  src/main.cpp src/multiplayer_server_lobby.* src/multiplayer_player_runtime.*
sed -n '360,570p' src/multiplayer_server_lobby.cpp
sed -n '145,205p' src/multiplayer_player_runtime.cpp
sed -n '540,650p' src/game.cpp
```

按四个可独立验收的 Linux-first slices 推进：

1. 先写 session 状态表和 API tests：将 lobby auth/resume 改为 pending request -> simulation-thread directory commit
   -> `complete_*` response；增加 directory-only exact `expected_old -> new` generation transition；由 resume token
   record 提供旧 generation。覆盖 pre-grace/grace/timeout 竞争、stale disconnect、重复 completion 和失败不发成功响应。
2. 对单玩家 active root 无法 offline 的冲突作出 ADR 级决定：neutral server root context，或 selected context 与
   runtime lifecycle 解耦。实现后固定“transport disconnect -> barrier disconnect -> forced wait -> terminal record ->
   runtime offline”的顺序。
3. 保持 `players.max = 1`，让 production owner 私有持有 scheduler/adapter，将 semantic command、forced timeout wait
   和 actual `process_legacy_single_player_bubble_turn()` 放进不可绕过的 claim/record 路径；把 execution fault 映射为
   typed fatal shutdown。只要可能已发生非幂等 gameplay side effect，就不得写新的 canonical save；只有明确分类为
   pre-side-effect 的 failure 才能正常保存。adapter 接管 action 时必须替换/绕过 legacy
   `execute_turn_player_action()` 外层 bookkeeping，不能把它直接包进现有 bool callback 导致双重记录。随后运行
   Linux `--server` + native client PTY smoke。
4. 将已绿色的 in-process two-runtime wait-only case 接入 production owner，再依次关闭 move/collision、monster/death、
   field/scent/NPC、tether/group shift 和 player-state isolation，最后才考虑 `players.max > 1`。

每个 slice 默认只跑 Linux focused tests；shared authority/session semantics 改变时增加完整 `[multiplayer]`，地址/
生命周期风险增加 sanitizer，production path 接通后增加 PTY loopback。只有 public compiler/ABI boundary、明确
Windows/Android-owned code 或 Phase 3 exit 才升级到对应 Tier 2/3；不得把全平台 package 作为日常前置。

不得把本批单客户端 UI smoke 描述为两玩家/shared-barrier、完整 remote avatar replica、portable character 或生产发布完成。
