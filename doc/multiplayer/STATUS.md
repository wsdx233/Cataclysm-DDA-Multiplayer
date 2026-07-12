# CDDA 多人 fork 当前状态

- 更新日期：2026-07-12
- 分支：`multiplayer/main`
- 当前阶段：Phase 0，基线、ADR 与可行性验证
- 当前 HEAD：`0955d865ea170c26511a789ce9d37334ac530953`，工作区尚未提交
- 上游基线：`d84b90dd2aee090ca28c8dad5cdf1fab6dea151a`

## 已完成并有证据

- Linux curses 本地基线包已通过构建、归档、`--version` 和动态库检查。
- Android arm64 debug 与 unsigned release APK 已通过本地构建、ZIP、ABI、badging 和签名状态检查。
- Linux/Android 本地环境门禁 `./build-scripts/check-multiplayer-build-env.sh all` 通过。
- 基线 workflow 通过 actionlint 1.7.12；新增 Bash 脚本通过 `bash -n` 和 ShellCheck 0.11.0。
- 九项 Phase 0 ADR 已建立；transport/TLS 与 player bridge 保持 `待验证`，其余七项为 `已接受`。
- 多人文档已统一到 `doc/multiplayer/`，文档职责与交接规则已写入 `AGENTS.md`。
- 地址稳定的 human-player registry、`player_runtime` sidecar 和完整活动玩家 guard 已在 GCC 13
  release/curses 与联合 sanitizer 配置下通过当前正向矩阵。

## Fork 完整基线 CI

首个 hosted run 是 GitHub Actions run `29177657248`（run number `#1`），源码提交为
`0955d865ea170c26511a789ce9d37334ac530953`：

- translations、tileset、soundpack 和 shaders 准备作业成功。
- Linux job `86609749923` 成功；artifact `cdda-linux-curses-x64-baseline`，ID `5623535391`，
  GitHub artifact digest
  `sha256:47a5e4a7ca9af1cf9b9abe0b7df69cb89deab7867db672ef55778a870ec273eb`。
- Android job `86609749928` 成功；artifact `cdda-android-arm64-baseline`，ID `5623530994`，
  GitHub artifact digest
  `sha256:ea104010c0c6152f6aa8b01a764717354fbd9f1ad9abf238aece18fba144c971`。
- Windows job `86609749930` 的 MSVC build 为 `0 Error(s)`，`windist.ps1 -SDL3` 完成；失败发生在
  `Smoke test and record provenance`。认证 job log 显示失败消息中的 exit code 为空：PowerShell
  直接调用 GUI subsystem 的 `cataclysm-tiles.exe` 后 `$LASTEXITCODE` 为 null，旧检查把它误判为非零。
  因 step 在 upload 前失败，该 run 没有 Windows artifact。
- workflow 已在当前工作区改用 `Start-Process -Wait -PassThru`，独立重定向 stdout/stderr，检查显式
  `ExitCode`，并要求版本文本包含 `Cataclysm`。修改后 actionlint 1.7.12 与 `git diff --check` 通过。

Windows 编译和打包已有 hosted runner 证据，但完整三端基线仍未通过：smoke 修复必须随新提交运行一次
`windows-2022` job，并成功上传 Windows artifact 后才能关闭该门禁。

## Player bridge 当前实现

- `game` 保留 legacy `u` backing avatar，并新增模拟线程 ID、可重定向的活动 avatar 借用指针、成对的
  `shared_ptr_fast<avatar>` owner 和 `multiplayer_player_registry`。
- registry 以 shared owner 保证 runtime/avatar 地址稳定并验证真实 control block；有效
  `character_id`、绝对位置和 `player_id` 可查询。2 到 4 个玩家的 ID/位置快照在查询时检测变化并重建，
  普通移动不会留下永久陈旧索引。
- `multiplayer_player_runtime` 为每名玩家保存 RFC 4122 v4 UUID `player_id`、递增
  `session_generation`、`importing/active/offline/dead` 状态，以及独立 messages、stats、achievements
  和 safe-mode 状态。
- session 状态转换是 runtime 私有 API，只能经 registry/game 执行；注册只接受 `importing`，注册成功后
  才启动 session，失败会回滚。任一 active guard 存续时禁止 registry 生命周期转换；当前活动玩家也不能
  被断线或标死，任何仍为 `active` 的 session 都不能直接注销。公开入口在 debug/sanitizer 下断言模拟线程，
  并在 release 下显式拒绝非模拟线程调用。
- `multiplayer_active_player_guard` 必须接收 registry 的真实 avatar owner，成对切换 avatar/runtime、消息日志、
  stats/achievements event-bus subscriptions 和 safe-mode 状态；支持严格 LIFO 嵌套、提前返回和异常恢复，
  不 move-swap `avatar` 或 `Character`。
- `get_avatar()`、`get_player_character()`、`get_player_view()`、`creature_at()`、`critter_by_id()`、
  `shared_from()`、`all_creatures()` 和 `num_creatures()` 已读取 human-player registry；`all_npcs()` 与
  `active_npc` 保持独立，因此 human avatar 不进入 NPC AI。
- 旧单人存档字段 `run_mode`、`mostseen`、`turnssincelastmon`、`stats_tracker` 和
  `achievements_tracker` 名称及 JSON 结构不变，数据源改为当前 runtime。双玩家 avatar/world save/load
  尚未实现，因此这只构成单人兼容证据。
- 完整 avatar move-swap 保留为负面对照：avatar JSON 可以 round-trip，但持久 wielded
  `item_location` 会失效，character-backed location 的 cached carrier 会指向错误对象槽和角色 ID。

## 本地验证

Release/curses 构建命令：

```bash
source build-scripts/activate-multiplayer-build-env.sh
make -j8 \
  COMPILER=g++-13 TILES=0 SOUND=0 RELEASE=1 LOCALIZE=0 \
  BACKTRACE=0 PCH=0 ASTYLE=0 tests
make -j8 \
  COMPILER=g++-13 TILES=0 SOUND=0 RELEASE=1 LOCALIZE=0 \
  BACKTRACE=0 PCH=0 ASTYLE=0
./cataclysm --version
```

结果：

- 全部受影响源码、测试和真实 `cataclysm` 在 `-Werror` 下编译链接；版本为 `0955d86-dirty`，
  `-tiles, -sound`。
- `./tests/cata_test '[stable_context]' --user-dir test_user_dir_multiplayer_runtime_final_release
  --rng-seed 0`：1 个用例，130/130 assertions 通过，包含 10,000 次上下文切换。
- `[stats]`：12 个用例，261/261 assertions 通过；`*message*`：3 个用例，27/27 通过；
  `*save*`：9 个用例，26/26 通过；`[force_load_game]` 通过。
- AStyle 3.1 使用仓库 `.astylerc` dry-run 检查本批 C++ 文件均为 `Unchanged`；actionlint 1.7.12
  检查 workflow 通过；`git diff --check` 通过。

联合 ASan/UBSan/LeakSanitizer 构建与运行命令：

```bash
source build-scripts/activate-multiplayer-build-env.sh
make -j6 \
  COMPILER=g++-13 TILES=0 SOUND=0 RELEASE=0 LOCALIZE=0 \
  BACKTRACE=0 PCH=0 ASTYLE=0 \
  BUILD_PREFIX=asan-ubsan- SANITIZE=address,undefined tests
ASAN_OPTIONS='detect_leaks=1:detect_stack_use_after_return=1:halt_on_error=1:abort_on_error=1' \
UBSAN_OPTIONS='print_stacktrace=1:halt_on_error=1' \
  ./tests/asan-ubsan-cata_test '[stable_context]' \
  --user-dir test_user_dir_multiplayer_runtime_final_stable_san --rng-seed 0
ASAN_OPTIONS='detect_leaks=1:detect_stack_use_after_return=1:halt_on_error=1:abort_on_error=1' \
UBSAN_OPTIONS='print_stacktrace=1:halt_on_error=1' \
  ./tests/asan-ubsan-cata_test '[.multiplayer_player_slot]' \
  --user-dir test_user_dir_multiplayer_runtime_final_hidden_san --rng-seed 0
```

结果：

- 正向稳定上下文为 130/130 assertions 通过。
- 完整隐藏组为 155 个 assertions 中 152 个通过、3 个 full-swap assertions 按预期失败；对应 2 个
  `!mayfail` 负面对照用例，正向用例无逻辑失败。
- 上述运行无 ASan、UBSan、LeakSanitizer 或 stack-use-after-return 报告。
- 联合 sanitizer 在早期验证中发现并修复四项独立的上游启动路径 UB：无 ImGui client 时的颜色元数据通知、
  actor 注册原型中的未初始化 `bool`、`item::browsed` 未初始化，以及 math parser 为静态参数绑定空
  dialogue 引用。修复后相关启动路径不再产生 sanitizer 报告。

## 已知限制与未验证项

- `game` 内仍有大量直接 `u` 访问；当前 guard 只证明 getter/registry 边界可行，不是完整多人规则执行环境。
- `player_id` 当前只在 runtime 生命周期内稳定，尚未进入 server save/schema；server restart 后的持久身份恢复
  和 portable-character ID remap 仍待设计实现。
- registry 位置索引尚未接入统一 movement/map-shift 事件；同格 mount/vehicle passenger 占位语义未定义。
- vehicle passenger、驾驶、mount、grab、remote control、missions、map memory、diary、recipes、bionics、
  mutations 和双玩家 save/load 尚未覆盖。
- 当前 C++ sidecar 改动只在 Linux GCC 13 验证；Android NDK 和 Windows MSVC 编译尚未验证。
- Windows workflow smoke 修复尚未经过 hosted rerun，完整基线不能标绿。
- 还没有 transport、TLS、协议、服务器主循环、客户端 scene/state 同步或 headless server target。
- ADR-0005 因上述边界矩阵和跨平台缺口继续保持 `待验证`。

## 下一步，按顺序

1. 将 Windows smoke 修复随当前工作提交到 fork，运行新的 `multiplayer-baseline` workflow；要求 Windows
   version smoke、`7z t`、provenance 和 artifact upload 全部成功，并记录 artifact ID/digest。
2. 将 registry 更新接入统一 movement/map-shift 边界，明确 mount、vehicle passenger、驾驶、grab 和
   remote-control 的玩家身份及占位语义，扩展 sanitizer 矩阵。
3. 实现/验证持久 `player_id` 与双玩家 server save/load，再覆盖 missions/map memory 等剩余 player-scoped
   状态。
4. 在 Android NDK 与 Windows MSVC 编译当前 player bridge；满足矩阵后对 ADR-0005 作 go/no-go 决策。
5. player bridge 决策和三端基线通过后，开始 standalone Asio/TCP 三平台 transport/TLS spike。

第一门禁应先检查 [multiplayer-baseline.yml](../../.github/workflows/multiplayer-baseline.yml) 的 Windows
smoke step；提交前执行：

```bash
actionlint .github/workflows/multiplayer-baseline.yml
git diff --check
git diff -- .github/workflows/multiplayer-baseline.yml
```

基线 rerun 通过后，玩家边界调查的第一条命令是：

```bash
rg -n "is_mounted|in_vehicle|controlling_vehicle|grab_type|remoteveh|update_map|shift" \
  src/game.cpp src/avatar*.cpp src/character*.cpp src/vehicle*.cpp
```

## 当前工作区说明

- 当前新增/修改内容尚未提交；旧 `doc/MULTIPLAYER_*.md` 的删除与 `doc/multiplayer/` 新文件是尚未提交的
  目录迁移，不要恢复旧路径。
- `obj/`、`asan-ubsan-obj/`、静态库、游戏和普通测试二进制是忽略的本地构建产物。
- 本轮 `tests/asan-ubsan-cata_test` 与 `test_user_dir_multiplayer_runtime_*` 临时验证产物已删除；后续
  sanitizer 运行生成的同类文件也不得提交。
