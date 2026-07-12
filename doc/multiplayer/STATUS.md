# CDDA 多人 fork 当前状态

- 更新日期：2026-07-12
- 分支：`multiplayer/main`
- 当前阶段：Phase 1，headless 运行模式与协议骨架
- 当前 HEAD：本文件所在提交；已验证并推送的父提交为
  `a39e06eb8620b377f515b6a8a7c8731b30543ebe`
- 上游基线：`d84b90dd2aee090ca28c8dad5cdf1fab6dea151a`

## Phase 0 关闭结论

Phase 0 的三项退出门禁均有完成证据，ADR-0003 和 ADR-0005 已从 `待验证` 更新为 `已接受`：

- 地址稳定的 avatar/player-runtime bridge 作出 **go** 决策。正向 release 与联合 sanitizer 矩阵均为
  274/274 assertions；完整 avatar move-swap 继续保留恰好 3 个预期 `!mayfail` identity 失败。
- standalone Asio/TCP spike 在 Linux GCC 13、Clang 18、Windows MSVC 和 Android NDK arm64 通过。
- Linux/Android/Windows 完整 baseline artifacts 在同一个提交上构建、上传并完成独立内容核对。

该结论不表示生产 transport、嵌入式 TLS、服务器循环、远程客户端或 canonical generation save 已完成。
`tools/multiplayer/transport_spike/` 保持隔离，只是可行性和回归工具。

## Hosted 构建证据

完整 baseline run `29205262759`（run number `#5`，提交 `a39e06e`）全部成功：

| 平台 | Job | Artifact ID | GitHub digest |
| --- | ---: | ---: | --- |
| Linux curses x64 | `86683564355` | `8263767548` | `4359fd56ddcb6948da9fffc38a5e37bf2a2457b748715aad30cdfebe5797235e` |
| Android arm64 | `86683694535` | `8263779993` | `414789e43381505c3dbcca5efe5b5f4acca21ccdce6cf989fe7a29a4470cc14a` |
| Windows client x64 | `86683694524` | `8263854364` | `dc9ee8104ee91c6195ad6a566961a4a97010f6fa4b6527f6bca74476c0c91d97` |

下载到 `/tmp` 后核对结果：

- Linux tar 内部 SHA-256 为
  `48f804153aa611350da3cbefaa474ce8e954b07a210184d480560180af3cde02`，9,291 个 entries，
  version smoke 为 `-tiles, -sound`。
- Android APK 内部 SHA-256 为
  `9b806ffab240500efd0776dcfab1954f9ce5ea611648ac36337c6415fecf0b99`；所有 6 个原生库均位于
  `arm64-v8a/`，其中 `libmain.so` 是 ELF64 AArch64。APK 包含 `android.permission.INTERNET`，且按设计未签名。
- Windows 内层 ZIP SHA-256 为
  `09eab6ee7c4e74dcbfa0d201d9d46d438485706ba3fc34fb58df1ca8aa1f984c`，含 9,048 个 files；
  `cataclysm-tiles.exe` 是 PE32+ x86-64 GUI executable。原生 `--version` 输出在 CRLF 归一化后逐行
  精确包含 `+tiles, +sound`。

Transport spike run `29205262750`（提交 `a39e06e`）三个 jobs 全部成功：

| 平台 | Artifact ID | GitHub digest | 内部 binary SHA-256 |
| --- | ---: | --- | --- |
| Linux GCC/Clang | `8263559175` | `f4d6e90086264b4989f0b6d21f4156caff15b7eb862cfa18dcbd025771e0cce4` | GCC `073dc02c23141c5a98fb395a4ef5bd0004e36cfb7a5646ad7666fbba3b31c0db` / Clang `a0603778b09ad1fcb6dcde7c2c777ff1d7386f12ea562999c33c80c8d789ad42` |
| Android arm64 | `8263560332` | `bbbba83a2053e4cc2a49ea43abba6b9306d70f19900553cf086bc1b659c9c197` | `5065fdb41b6ec45f18d165d28bf5e36d75b6e7bc4c58e10980a3c669bc380b46` |
| Windows MSVC | `8263560944` | `f9dd8750b4543f39d24e1efc1da327fc77afc71db7856f1423956d0996a04aa1` | `5c974a5459acab28267b8a5b067220d6942ff18de78d76ed9972ca88229343fb` |

Linux GCC/Clang 与 Windows MSVC 原生 loopback 均报告：

```text
transport spike passed: fragmentation, coalescing, half-close, cancellation, queue-full, ordered-shutdown
```

Asio 固定为 `1.38.1` / `asio-1-38-1` / commit
`dfd7b3e3145bac5d0e91a99fde69c6ae1442f971`，archive SHA-256 为
`2827b229972be80cdb14e5497962fa393d1adf036b5869e2b9c99f644daadacc`，许可证为 BSL-1.0。

## Player Bridge 实现

- `multiplayer_player_registry` 以真实 shared ownership 持有地址稳定的 runtime/avatar，并索引稳定
  `player_id`、`character_id` 和位置；human avatar 参与 creature 查询但不进入 NPC AI。
- `multiplayer_player_runtime` 持有 RFC 4122 v4 `player_id`、session generation、
  `importing/active/offline/dead` 生命周期、messages、stats/achievements、safe mode 和 remote vehicle cache。
- `multiplayer_active_player_guard` 只允许模拟线程在安全点使用，成对切换完整玩家上下文，支持严格 LIFO
  嵌套、提前返回和异常恢复，不 move-swap `avatar`/`Character`。
- movement、teleport、`map::shift()`、mount、vehicle passenger/driver、grab 和 remote control 保持多玩家
  身份。map shift 同步全部 runtime 的 route/remote 坐标，vehicle cache rebuild 后按玩家重新解析。
- player snapshot schema version 1 严格验证 UUID、character ID、safe mode、schema 和 generation exhaustion；
  加载 runtime 保持 `importing`，registry 完成唯一性验证后才开始新 session。
- 单人存档的旧字段名和 JSON 结构保持不变。player snapshot 只是 Phase 0 边界，不是 ADR-0007 的
  canonical generation world save。

## `do_turn()` 阶段证据

新增 `multiplayer_turn_phase` 的线程局部可选 observer，真实 `game::do_turn()` 依次记录：

1. `turn_begin`
2. `player_begin`
3. `player_input`
4. `world`
5. `player_end`

observer 未安装时只有固定数量的空指针分支且不读取 `steady_clock`，不改变 turn 控制流。release 和联合
ASan/UBSan/LSan 的 `[turn_phase]` 测试均为 8/8 assertions。禁用 autosave 后的单次本地诊断样本约为
`0.048 / 0.017 / 0.011 / 3.339 / 0.131 ms`；这不是 p95，也不是性能预算。

源码审计在记录时找到 1,354 个 active-player getter 匹配，分布在 159 个文件。高风险 world-phase 类别
包括 monster/NPC target、scent、field、vehicle、sound、mission、timed event 和 visibility；具体所有权与
后续门禁见 `DO_TURN_PHASE_AUDIT.md`。

## 本地验证

Release bridge 回归：

```bash
./tests/cata_test '[stable_context]' \
  --user-dir test_user_dir_multiplayer_turn_phase_stable_release --rng-seed 0
```

结果：1 个用例，274/274 assertions 通过。

Turn phase release 与 sanitizer：

```bash
./tests/cata_test '[turn_phase]' \
  --user-dir test_user_dir_multiplayer_turn_phase_release --rng-seed 0

ASAN_OPTIONS='detect_leaks=1:detect_stack_use_after_return=1:halt_on_error=1:abort_on_error=1' \
UBSAN_OPTIONS='print_stacktrace=1:halt_on_error=1' \
./tests/asan-ubsan-cata_test '[turn_phase]' \
  --user-dir test_user_dir_multiplayer_turn_phase_sanitizer --rng-seed 0
```

结果：两次均为 8/8 assertions；sanitizer 无 ASan、UBSan、LSan 或 stack-use-after-return finding。

Android NDK 当前工作树：

```bash
cd android
./gradlew -Pj=6 \
  -Pabi_arm_32=false -Pabi_arm_64=true \
  -Pabi_x86_32=false -Pabi_x86_64=false \
  -Plocalize=false assembleExperimentalDebug
```

结果：`BUILD SUCCESSFUL in 14m 59s`，39 tasks（14 executed、25 up-to-date）。新 phase observer 和
`do_turn.cpp` 均进入 arm64 native target。

本批 C++ 文件经 AStyle 3.1 dry-run 均为 `Unchanged`；`git diff --check` 通过。此前受影响回归结果仍为
stats 261、messages 27、saves 26、map/teleport/vehicle/load 139,624 assertions 全部通过。

## 当前限制

- 尚无 `runtime_mode`、server CLI/config、headless startup、生产 transport、FlatBuffers schema、握手、
  content manifest、server loop 或 network client。
- 没有嵌入式 TLS backend。默认只能 plaintext loopback；受信 LAN 必须显式启用，WAN 必须使用
  WireGuard/Tailscale 等外部认证加密隧道。
- phase observer 只提供测试和 profiling 边界，没有把 `do_turn()` 改造成多人 scheduler。
- world phase 仍有大量隐式 active-player getter；必须按审计逐组显式化，不能把 guard 当作“所有世界逻辑
  已支持多人”的证明。
- player snapshot 不保存 world、RNG 或 generation。server restart consistency 仍属于 Phase 5。
- 本文件所在提交的新 phase observer 已在本地 GCC/Android NDK 验证；提交后的 hosted MSVC/baseline
  rerun 尚需记录，不能把父提交 artifact 当成新文件的 MSVC 编译证据。

## 下一步，按顺序

1. 在 `src/multiplayer_runtime_mode.*` 和 `src/multiplayer_server_config.*` 增加可测试的 runtime mode、严格
   versioned JSON config、安全 listener 校验，以及 `--server`、`--check-server-config`、
   `--init-server-config` CLI。
2. 重构 `src/main.cpp` startup，使 dedicated server 跳过 curses、SDL/ImGui、sound、popup、language menu
   和 main menu，同时保留静态数据加载、结构化错误输出和 signal-driven graceful shutdown。
3. 将 standalone Asio 以固定生产依赖边界接入游戏构建；不要链接或复制 spike main。实现有界 frame/
   connection transport 和 in-process loopback server/two-client harness。
4. 增加 pinned FlatBuffers schema、generated-header check、version/capability handshake、content manifest 和
   不兼容客户端拒绝测试。

第一条代码调查命令：

```bash
rg -n "cli_opts|parse_commandline|init_interface|init_ui|cataimgui|opening_screen|exit_handler" \
  src/main.cpp src/game_ui.cpp src/cata_imgui.cpp
```

提交后先运行并记录新的 hosted baseline；第一个实现文件应从 `src/main.cpp`、
`src/multiplayer_runtime_mode.*` 和 `src/multiplayer_server_config.*` 开始。

## 工作区说明

- `test_user_dir_*`、`tests/asan-ubsan-cata_test`、Android Gradle/CMake outputs 和其他对象/包是忽略或未跟踪
  的验证生成物，不得提交，也不要在没有用户指示时批量删除。
- 所有下载的 CI artifacts 都位于 `/tmp/multiplayer-*-run-*`，不在仓库中。
