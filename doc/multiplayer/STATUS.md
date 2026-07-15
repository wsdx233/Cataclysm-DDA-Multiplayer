# CDDA 多人 fork 当前状态

- 更新日期：2026-07-15
- 分支：`multiplayer/main`
- 当前 implementation source：`2e9236c7bf91782ad3f15daa5d8aa0e3929f355a`
- 上游基线：`d84b90dd2aee090ca28c8dad5cdf1fab6dea151a`
- 当前阶段：**Phase 3，第二玩家/shared scheduler**
- 当前 active gate：**Gate 3，two-runtime owner 与共享规则矩阵**
- 已关闭 gate：Gate 1 selected-root lifecycle contract；Gate 2 production single-root lifecycle
- ADR 状态：ADR-0001 至 ADR-0011 均已接受
- 配置约束：`players.max = 1`；Gate 3 owner/rule/process gates 关闭前不得提高
- 当前验证策略：**Tier 1 Linux-first**；本批 Windows/Android 按策略未运行

## 当前结论

Phase 3 Gate 2 已由 source `2e9236c7bf91782ad3f15daa5d8aa0e3929f355a` 关闭。production single-player
dedicated server 现在由 `multiplayer_single_root_owner` 在 simulation thread 私有持有 authoritative session
directory、turn scheduler、selected-root lifecycle 和 phase adapter：

- auth/resume 继续使用两阶段 admission 和 exact generation owner；admission plan、directory commit、transport
  publish receipt、unpublished cleanup、scheduler/runtime effect 与 lifecycle record 属于同一个 owner transaction；
- semantic wait/move 与 disconnect forced wait 经 adapter/active-player guard 执行，action bookkeeping 恰好一次；
- `game::do_turn_remote_owned()` 提供 mandatory player-phase completion，sleep/activity/zero-moves 不能绕过 terminal
  transition；
- actual `process_legacy_single_player_bubble_turn()` 只能经 exact world claim 的 one-shot thunk 执行；scheduler world
  completion 位于 world hook 内，lifecycle completion 等 player-end 完成并返回后才记录；
- root disconnect 超过 grace 后按 `forced wait -> terminal -> world -> player-end -> exact offline -> dormant` 推进；
  dormant 不开始 turn、不创建 guard、不执行 command，resume 先 re-activate 同一稳定 runtime；
- ADR-0011 的 outer pump 处理 initial/dormant/between-turn admission；只有 open player-input、world-before-claim 的
  同步 wait 可运行 bounded active-turn pump；world/player-end 不泵 control；
- open-turn resume/resync 只重放上一次 immutable completed-scene payload。command result 使用最近 completed scene
  revision；world + player-end + lifecycle completion 后才推进 revision、发布 scene、保存或开始下一 turn；
- graceful disconnect 明确区分 ACK `queued`、fallback close、stale request 和 fatal result；connection 一旦决定关闭，
  同批或已排队 semantic command 会在 gameplay 前被丢弃；
- owner 的 safe-boundary disposition 控制 shutdown/save。open turn 或 post-side-effect canonical state 不可证明时
  typed fatal/no-save；clean completed boundary 或 dormant 才允许 canonical save。

这些结论只证明 production **single-root** lifecycle，不是 two-player routing。`players.max` 必须保持 `1`。

## 当前 ownership 快照

| 层 | 当前 owner/已完成边界 | Gate 3 缺口 |
| --- | --- | --- |
| Transport/lobby | connection、token mirror、pending admission、typed ordered close/rejection、closing priority | 双连接 admission/routing 仍未开放 |
| Session directory | stable identity/binding/generation、two-stage commit/confirmation、graceful pending、exact offline/reactivation | multi-runtime roster transition 尚未接入 production owner |
| Registry/runtime | 地址稳定 avatar/runtime、active/offline/dead、active-player guard | 多 runtime 的安全 selected-context 切换与 death policy |
| Scheduler | immutable roster、round-robin、disconnect grace、forced-wait pending、world ticket、fault latch | production two-runtime roster 与公平 command routing |
| Phase adapter | scoped action/wait/bookkeeping、world claim callback、post-effect fail-stop | 第二 runtime 的完整规则矩阵 |
| Single-root owner | directory/scheduler/lifecycle/adapter、dormant/resume、safe save disposition | 固定拒绝 `maximum_players != 1`，不得直接扩容 |
| Dedicated server | outer/active pump、completed-scene cache、typed fatal/no-save、真实 curses/headless loop | test-only 双连接开关和双 client smoke/soak 尚未实现 |

## Gate 2 本批变化

### Owned turn 与 scheduler

- 新增 `multiplayer_turn_scheduler::record_player_phase_completed()`，为无 action callback 的 player phase 提供显式
  terminal transition，不伪造 command result。
- 新增 typed `multiplayer_owned_remote_turn_hooks/result/token` 和 one-shot
  `multiplayer_owned_world_thunk`。completion token 绑定 owner/shared-turn/invocation，公开字段不能伪造或重放前一 turn
  的 player-end proof。
- adapter-owned action 绕过 legacy `execute_turn_player_action()` 外层 bookkeeping，避免双重记录。
- `can_execute_command()` 同时要求 lifecycle commandable 和 scheduler exact current slot `awaiting_command`。

### Production owner 与 lifecycle

- 新增 `src/multiplayer_single_root_owner.h/.cpp`；construction 明确要求 simulation thread 和
  `maximum_players == 1`。
- exact admission receipt、dormant-origin unpublished cleanup、same-generation replay、grace `+1` repair、graceful
  release pending、early close、forced wait/world/offline/dormant 和 same-runtime resume 均由 owner 组合。
- 任何 external effect 已成功但 matching lifecycle record 非 applied/duplicate 时立即 latch fault；若 gameplay/world
  side effect 可能已发生，则 canonical state 标记 uncertain 并拒绝新 save。

### Server loop、scene 与 close priority

- `run_dedicated_server()` 的 pump 已移出 legacy action callback。initial unbound/dormant 可 auth/resume 而不先进入
  turn；active-turn pump 只存在于 owned player-input wait。
- scene revision 表示 completed publish boundary。open barrier 的 resume/resync 换 exact session envelope 后重放缓存
  payload，不读取 partial live game state。
- lobby 在更新 inbound high-water/confirmation 前先完整验证 resync payload；main 在 authoritative confirmation 前
  校验 requested revision。
- `multiplayer_dedicated_server::connection_is_closing()` 跟踪所有 wrapper/lobby/transport/graceful/rejection close
  决策。合法 command 与 malformed control 同批到达时，合法 frame 可完成 tuple confirmation，但不会进入 scene/
  queue/gameplay；terminal disconnected event 消费后清除 closing state。

## 当前 source 验证证据

### Linux build、release tests 与格式

本批使用当前 workspace Linux toolchain；实现工作树在无后续 source 修改的情况下提交为
`2e9236c7bf91782ad3f15daa5d8aa0e3929f355a`。Gate 2 process/full-suite binary 在提交动作前生成，内嵌 build ID 因此
仍显示 `fb3ca50-dirty`；提交前 staged source diff 与所验证工作树一致，提交后没有再修改 source。source commit 后
以下 build 命令又完整成功，当前 binary build ID 为 `2e9236c7bf91782ad3f15daa5d8aa0e3929f355a-dirty`，其中
`dirty` 只来自本次文档收口：

```bash
source build-scripts/activate-multiplayer-build-env.sh
./build-scripts/check-multiplayer-build-env.sh linux

make -j"$(nproc)" AUTO_BUILD_PREFIX=1 \
  COMPILER=g++-13 RELEASE=1 LOCALIZE=0 BACKTRACE=0 PCH=0 ASTYLE=0 \
  tests release-local-back-cataclysm

./tests/release-local-back-cata_test \
  '[multiplayer][single_root_owner]' \
  --rng-seed 0 --user-dir /tmp/cdda-mp-gate2-owner-final

./tests/release-local-back-cata_test \
  '[multiplayer][phase_adapter],[multiplayer][scheduler]' \
  --rng-seed 0 --user-dir /tmp/cdda-mp-gate2-turn-scheduler-final

./tests/release-local-back-cata_test \
  '[multiplayer][dedicated_server],[multiplayer][server_lobby]' \
  --rng-seed 0 --user-dir /tmp/cdda-mp-gate2-server-final

./tests/release-local-back-cata_test '[multiplayer]' \
  --rng-seed 0 --user-dir /tmp/cdda-mp-gate2-full-final-20260715

make \
  ASTYLE_BINARY="$HOME/.cache/cdda-tools/astyle-3.1-3build1/root/usr/bin/astyle" \
  astyle-check
git diff --check
```

结果：

- Linux environment gate、GCC 13 release tests 和 production curses/server binary 构建成功。
- single-root owner：11 cases / 374 assertions，全过。
- phase adapter + scheduler：28 cases / 1,104 assertions，全过。
- dedicated server + lobby：16 cases / 3,549 assertions，全过。
- 完整 `[multiplayer]`：125 cases；123 passed + 2 个既有 `[!mayfail]` full-avatar move-swap identity 负面对照；
  7,174 assertions 中 7,171 passed + 3 expected failures；exit 0。
- AStyle 3.1 和 `git diff --check` 通过。

headless protocol client 另以 production sources 和 warning-as-error 构建：

```bash
g++-13 -std=c++17 -O2 -Wall -Wextra -Werror \
  -Isrc -isystem src/third-party \
  tools/multiplayer/headless_client_smoke.cpp \
  src/multiplayer_protocol.cpp src/multiplayer_transport.cpp \
  src/multiplayer_crypto.cpp -pthread \
  -o build/multiplayer-smoke/headless_client_smoke
```

结果：编译成功，无 warning。

### Linux sanitizer

```bash
source build-scripts/activate-multiplayer-build-env.sh
ASAN_OPTIONS='detect_leaks=1:detect_stack_use_after_return=1:halt_on_error=1:abort_on_error=1' \
UBSAN_OPTIONS='print_stacktrace=1:halt_on_error=1' \
./tests/release-local-back-sanitize-cata_test \
  '[multiplayer][single_root_owner],[multiplayer][dedicated_server],[multiplayer][server_lobby]' \
  --rng-seed 0 --user-dir /tmp/cdda-mp-gate2-sanitize-final-source
```

结果：27 cases / 3,923 assertions，全过；无 ASan、UBSan、LSan 或 stack-use-after-return finding。此前更宽的 Gate 2
sanitizer batch 也覆盖 phase adapter/scheduler；最终 localized close-priority 和 owner-commandability 修复后，以上
affected final-source sanitizer 重新运行并绿色。

### Linux headless process smoke

配置根：`build/multiplayer-gate2-headless-final2-20260715`，loopback port `43205`，
`disconnect_grace_seconds = 3`。

```bash
set -euo pipefail
root="$PWD/build/multiplayer-gate2-headless-final2-20260715"
port=43205
rm -rf "$root"
mkdir -p "$root"
./release-local-back-cataclysm \
  --userdir "$root/user" --init-server-config "$root/server.json" \
  >"$root/init.stdout" 2>"$root/init.stderr"
jq --arg listen "127.0.0.1:$port" \
  '.network.listen = $listen | .players.disconnect_grace_seconds = 3' \
  "$root/server.json" >"$root/server.json.tmp"
mv "$root/server.json.tmp" "$root/server.json"

stdbuf -oL -eL ./release-local-back-cataclysm \
  --userdir "$root/user" --server "$root/server.json" \
  >"$root/server.stdout" 2>"$root/server.stderr" &
server_pid=$!
for _ in $(seq 1 600); do
  rg -q '"event":"listening"' "$root/server.stdout" && break
  kill -0 "$server_pid"
  sleep 0.1
done
rg -q '"event":"listening"' "$root/server.stdout"

build/multiplayer-smoke/headless_client_smoke \
  127.0.0.1 "$port" "$root/server-auth-token.txt" \
  >"$root/client.stdout" 2>"$root/client.stderr"
sleep 5
kill -TERM "$server_pid"
wait "$server_pid"

test "$(rg -c '"event":"player_resumed"' "$root/server.stdout")" -eq 3
test "$(jq -r 'select(.event == "command_result") | .status' \
  "$root/server.stdout" | paste -sd, -)" = '0,2,0'
rg -q 'open-barrier cached resume/resync' "$root/client.stdout"
rg -q 'revisions 1 -> 2 -> 3' "$root/client.stdout"
rg -q '"event":"command_sequence_conflict"' "$root/server.stdout"
rg -q '"event":"save_completed".*"revision":"4"' "$root/server.stdout"
rg -q '"event":"shutdown"' "$root/server.stdout"
! rg -q '"event":"save_refused"|"event":"runtime_failed"' "$root/server.stdout"
```

结果：

- handshake rejection/acceptance、fresh auth、initial completed scene 和 resync 通过；
- zero-action open barrier raw close 后 generation 2 resume；resume scene 与上一次 completed/resync payload 逐字节一致；
  open barrier 再次 resync 仍逐字节一致；
- sequence 4 wait accepted，generation 3 resume 后 sequence 4 duplicate，再执行 sequence 5 fresh wait；
- generation 4 resume 后 changed-payload sequence-4 conflict 触发 terminal close；
- `player_resumed` 共 3 次；command statuses 为 `0,2,0`；completed revisions 为 `1 -> 2 -> 3`；
- 等待超过 grace 后 SIGTERM，server exit 0，记录 `save_completed(revision=4)` 和 `shutdown`；无
  `save_refused`/`runtime_failed`。

### Linux native curses client PTY

配置根：`build/multiplayer-gate2-ui-final-20260715`，backend port `43215`。

```bash
set -euo pipefail
root="$PWD/build/multiplayer-gate2-ui-final-20260715"
port=43215
rm -rf "$root"
mkdir -p "$root"
./release-local-back-cataclysm \
  --userdir "$root/server-user" --init-server-config "$root/server.json" \
  >"$root/init.stdout" 2>"$root/init.stderr"
jq --arg listen "127.0.0.1:$port" \
  '.network.listen = $listen | .players.disconnect_grace_seconds = 3' \
  "$root/server.json" >"$root/server.json.tmp"
mv "$root/server.json.tmp" "$root/server.json"

stdbuf -oL -eL ./release-local-back-cataclysm \
  --userdir "$root/server-user" --server "$root/server.json" \
  >"$root/server.stdout" 2>"$root/server.stderr" &
server_pid=$!
for _ in $(seq 1 600); do
  rg -q '"event":"listening"' "$root/server.stdout" && break
  kill -0 "$server_pid"
  sleep 0.1
done
rg -q '"event":"listening"' "$root/server.stdout"

python3 tools/multiplayer/network_client_ui_smoke.py \
  --client "$PWD/release-local-back-cataclysm" \
  --backend-port "$port" \
  --token-file "$root/server-auth-token.txt" \
  --user-dir "$root/client-user" \
  --transcript "$root/client.transcript" \
  --event-log "$root/client-events.txt" \
  --timeout-seconds 60 \
  >"$root/client.stdout" 2>"$root/client.stderr"
sleep 1
kill -TERM "$server_pid"
wait "$server_pid"

test "$(jq -r 'select(.event == "command_result") | .status' \
  "$root/server.stdout" | paste -sd, -)" = '0,2,0'
test "$(rg -c '"event":"player_resumed"' "$root/server.stdout")" -eq 1
rg -q 'sent local quit input' "$root/client.stdout"
rg -q 'client exited with status 0' "$root/client.stdout"
rg -q 'scene_sync_events=3' "$root/client.stdout"
rg -q '"event":"save_completed"' "$root/server.stdout"
rg -q '"event":"shutdown"' "$root/server.stdout"
! rg -q '"event":"save_refused"|"event":"runtime_failed"' "$root/server.stdout"
```

结果：真实 curses client 完成 wait、proxy 强制断开、generation-2 resume、uncertain command duplicate、east move 和
local quit；command statuses `0,2,0`，scene sync events `3`，client/server 均 exit 0；server clean save/shutdown，无
fatal/no-save event。

### Open-turn fatal/no-save process gate

配置根：`build/multiplayer-gate2-open-turn-nosave-final-20260715`，loopback port `43225`。真实 curses client 在 private
PTY 连接后不输入 action；server 观察到 `player_authenticated` 且无 `command_result`，随后收到 SIGTERM。

```bash
set -euo pipefail
root="$PWD/build/multiplayer-gate2-open-turn-nosave-final-20260715"
port=43225
rm -rf "$root"
mkdir -p "$root"
./release-local-back-cataclysm \
  --userdir "$root/server-user" --init-server-config "$root/server.json" \
  >"$root/init.stdout" 2>"$root/init.stderr"
jq --arg listen "127.0.0.1:$port" \
  '.network.listen = $listen | .players.disconnect_grace_seconds = 3' \
  "$root/server.json" >"$root/server.json.tmp"
mv "$root/server.json.tmp" "$root/server.json"

stdbuf -oL -eL ./release-local-back-cataclysm \
  --userdir "$root/server-user" --server "$root/server.json" \
  >"$root/server.stdout" 2>"$root/server.stderr" &
server_pid=$!
for _ in $(seq 1 600); do
  rg -q '"event":"listening"' "$root/server.stdout" && break
  kill -0 "$server_pid"
  sleep 0.1
done
rg -q '"event":"listening"' "$root/server.stdout"

client_cmd="$PWD/release-local-back-cataclysm --userdir $root/client-user/"
client_cmd+=" --connect 127.0.0.1:$port"
client_cmd+=" --connect-token-file $root/server-auth-token.txt"
setsid script -q -e -c "$client_cmd" "$root/client.typescript" \
  </dev/null >"$root/client.stdout" 2>"$root/client.stderr" &
client_pid=$!
for _ in $(seq 1 600); do
  rg -q '"event":"player_authenticated"' "$root/server.stdout" && break
  kill -0 "$server_pid"
  sleep 0.1
done
rg -q '"event":"player_authenticated"' "$root/server.stdout"
sleep 0.25
! rg -q '"event":"command_result"' "$root/server.stdout"

find "$root/server-user/save/coop-world" -type f -print0 | \
  sort -z | xargs -0 sha256sum >"$root/save.before"
kill -TERM "$server_pid"
set +e
wait "$server_pid"
server_status=$?
set -e
kill -TERM -- "-$client_pid" 2>/dev/null || true
wait "$client_pid" || true
find "$root/server-user/save/coop-world" -type f -print0 | \
  sort -z | xargs -0 sha256sum >"$root/save.after"

test "$server_status" -eq 1
cmp "$root/save.before" "$root/save.after"
! rg -q '"event":"command_result"|"event":"save_completed"' "$root/server.stdout"
rg -q '"event":"save_refused".*"disposition":"2"' "$root/server.stderr"
rg -q '"event":"runtime_failed".*owned turn did not reach an exact player-end lifecycle boundary' \
  "$root/server.stderr"
```

结果：server exit `1`；记录 `save_refused` disposition `2` 和
`runtime_failed("owned turn did not reach an exact player-end lifecycle boundary")`；没有 `save_completed`；退出前后
`server-user/save/coop-world` 全部文件 SHA-256 清单完全一致。client 在 server half-close 后由 harness 清理，未产生
gameplay command。

## 平台选择与未运行证据

本批选择 Tier 1，因为 source diff 是 backend-neutral internal gameplay/session/lifecycle implementation。审计命令：

```bash
git diff --name-status \
  cbd19b48d652be735a3c83fe841d2d7c831aecba..2e9236c7bf91782ad3f15daa5d8aa0e3929f355a
```

审计未发现 Android/Windows-owned source、platform conditional、wire schema/version/capability、generated protocol、
transport/crypto public boundary、shared source list、workflow、artifact 或 pinned toolchain 变化。`game.h` 的新增类型和
method 是 repo-internal owned-turn seam，不改变外部 wire/serialization ABI，也没有平台分支。故本批未运行 Windows
MSVC package、Android APK/NDK 或 emulator/device；这是按策略未运行，不是 blocker，也不构成当前 source 的平台证据。

最近兼容的历史证据：

- Gate 1 source `cbd19b48d652be735a3c83fe841d2d7c831aecba` 的 Linux production run `29392575820` terminal
  `success`；Windows/Android jobs 精确 skipped。
- protocol minor `1` public-boundary source `eb990c4ad9975915336f3acd65431b47d123e842` 的 baseline run
  `29385561653` 对 Windows/Android actual production source 编译绿色。它只覆盖未变化的 protocol boundary，不证明
  当前 Gate 2 source 在这些平台编译或运行。

## 已知限制

- `players.max = 1`；没有 production two-runtime owner、第二 client routing 或 shared round-robin process evidence。
- production wait/move 都消耗完整 standard move budget。owner exact test 已覆盖
  `accepted_remains_eligible -> second command`；未来任何自然保留 moves 的 executor 启用前，必须增加真实 process
  回归。
- production game-over/death 当前进入 typed fatal/no-save；monster targeting、player death 和可证明的 death save
  boundary 属 Gate 3。
- `game::walk_move()` 等路径仍有固定 `game::u` 假设；human collision、field/scent/NPC、tether/group shift、messages/
  safe-mode/stats/player-state isolation 未关闭。
- 当前远程动作只有 wait 和八方向平面 move；其他动作必须 typed unsupported，不能进入 blocking UI。
- scene 仍为 full snapshot；items、fields、vehicles、overlays、messages、sound、avatar replica/panels 和完整 visibility
  leak matrix 属 Phase 4/后续 gate。
- save 仍是 canonical single-avatar generation；durable process-restart resume、portable character 和 multiple save
  generations 属后续阶段。
- 无嵌入式 TLS；loopback 默认、trusted-LAN 显式例外和外部 authenticated tunnel 政策不变。

## 下一门禁与首个动作

当前 active gate 是 Gate 3。第一步不是修改 single-root owner 的 `maximum_players == 1` 约束，而是调查现有
two-runtime scheduler/adapter 测试，定义一个独立 multi-runtime owner contract：

```bash
rg -n 'two.runtime|two_runtime|maximum_players|begin_turn|execute_current_player|current_slot|round.robin' \
  tests/multiplayer_* \
  src/multiplayer_single_root_owner.* \
  src/multiplayer_turn_scheduler.* \
  src/multiplayer_turn_phase_adapter.*

sed -n '1,280p' src/multiplayer_single_root_owner.h
sed -n '1,360p' src/multiplayer_turn_scheduler.h
```

Gate 3 的 ordered slices：

1. 新建/定义 multi-runtime owner contract，并把现有 in-process two-runtime wait-only barrier 提升为 owner-level
   integration test；外部配置继续拒绝 `players.max > 1`。
2. round-robin wait/move 与 player-state isolation。
3. human collision。
4. monster target/attack 与 death/game-over safe boundary。
5. field/scent/NPC。
6. tether/group shift。
7. 仅在上述 owner/rule gates 绿色后，增加 test-only 双连接 routing 和 Linux two-client smoke/soak；之后才评估开放
   `players.max > 1`。

每个编辑循环只运行 Linux incremental build 和 changed-area focused tests。只有 coherent authority/lifecycle slice
收口时才增加完整 `[multiplayer]`、定向 sanitizer 或 Linux process smoke；只有已完成批次实际触及 Windows/Android
owned code、公共 wire/ABI/toolchain 或平台产品声明时，才运行对应平台的最小必要 gate。
