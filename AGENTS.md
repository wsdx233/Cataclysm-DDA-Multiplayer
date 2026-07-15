# CDDA Multiplayer Fork Agent Guide

## Scope And Sources Of Truth

This file applies to the entire repository.

Before changing multiplayer architecture or build/release behavior, read:

- [`doc/multiplayer/README.md`](doc/multiplayer/README.md): documentation map, reading order, ownership, and handoff rules.
- [`doc/multiplayer/STATUS.md`](doc/multiplayer/STATUS.md): current phase, active work, evidence, blockers, and exact next steps.
- [`doc/multiplayer/MULTIPLAYER_REFACTOR_PLAN.md`](doc/multiplayer/MULTIPLAYER_REFACTOR_PLAN.md): product scope, architecture, phases, risks, tests, and prohibited shortcuts.
- [`doc/multiplayer/MULTIPLAYER_BUILD_BASELINE.md`](doc/multiplayer/MULTIPLAYER_BUILD_BASELINE.md): pinned toolchains, CI artifacts, local setup, and verified build status.
- [`doc/multiplayer/adr/README.md`](doc/multiplayer/adr/README.md): accepted and pending architecture decisions.

The refactor plan is the source of truth for fork-specific architecture. Existing upstream CDDA patterns remain the default for code style and unchanged game behavior. If code investigation invalidates a plan assumption, record the decision in an ADR and update the plan instead of silently diverging.

## Documentation And Handoff Discipline

`AGENTS.md` is the operational entry point for future coding agents. Keep it concise and current: it should explain the project goal, non-negotiable engineering constraints, current phase gates, toolchain commands, and how work is handed off. Do not turn it into a chronological development diary or duplicate detailed ADR rationale here.

Use the multiplayer documentation by ownership:

- Put durable product scope, architecture, phase ordering, risks, and exit criteria in the refactor plan.
- Put one-way or difficult-to-reverse technical decisions, alternatives, consequences, and validation gates in ADRs.
- Put pinned toolchains, reproducible build commands, artifact contracts, and verified platform results in the build baseline.
- Put the current work snapshot, commands already run, concrete results, unresolved findings, and ordered next actions in `STATUS.md`.
- Put navigation and document-maintenance rules in `doc/multiplayer/README.md`.

At the start of a multiplayer task, read `AGENTS.md`, `STATUS.md`, the relevant plan sections, and every related ADR before editing code. At the end of a task, update the owned documents in the same change as the code when facts changed. A handoff is not complete unless `STATUS.md` states:

- the current branch and phase;
- what changed and why;
- exact verification commands and outcomes;
- known failures or unverified platforms;
- the next gate and the first concrete command or file to inspect.

Do not mark a gate complete based on intent, a POC compile, or an in-progress CI run. Record evidence first. If implementation findings contradict an accepted ADR, stop expanding the implementation, mark the conflict in `STATUS.md`, and update or supersede the ADR plus the refactor plan before continuing.

## Project Goal

This is a long-lived CDDA fork based initially on upstream commit `d84b90dd2aee090ca28c8dad5cdf1fab6dea151a` (`cdda-experimental-2026-07-11-0744`). The target product is:

- Android graphical client.
- Windows graphical client.
- Linux and Windows dedicated headless server.
- Direct connection by server address; no public lobby is required for v1.
- Two to four players sharing one server-authoritative world.

The architectural summary from the refactor plan is:

1. The server owns world state, characters, RNG, action validation, results, and canonical saves.
2. Clients keep local input, keybindings, touch controls, tilesets, fonts, language, panels, soundpacks, and window layout.
3. The network carries typed semantic commands and visibility-filtered scene/state data. It does not synchronize save directories or trust client-computed outcomes.
4. Simulation remains single-threaded. Network and compression workers may only exchange immutable DTOs through bounded queues.
5. Time uses a shared turn barrier with fair round-robin command execution.
6. v1 uses one shared reality bubble with a player-distance tether. Multiple reality bubbles are a later optional phase.
7. Each player avatar has stable registry-owned storage. Existing player getters are redirected through a simulation-thread-only active-player context; full avatar move-swap remains only as a failing reference-identity comparison.
8. Input parsing and rule execution must be separated. Single-player and multiplayer should call the same command executors.
9. Clients must not upload a complete `.sav`. Character transfer uses a versioned portable package with ID remapping and world-reference cleanup.
10. Headless execution must not initialize or block on SDL, curses UI, ImGui, sound, popups, or local menus.

See the plan sections on authority, threading, state scope, scheduling, action/UI separation, protocol, headless startup, security, and testing for the full requirements.

## Current Phase

Phase 0 closed with recorded evidence on 2026-07-12, and Phase 2 closed on 2026-07-14. The project is now in
**Phase 3: second-player/shared-scheduler implementation**. Current source includes the pure
`multiplayer_turn_scheduler`, a source `multiplayer_turn_phase_adapter` currently called only by tests, explicit
authoritative-wait mode, target action bookkeeping inside the active-player guard, persistent scheduler fail-stop,
and named legacy single-player world/action seams. Linux release and sanitizer tests cover exact
slot/generation/ownership/root-context validation,
the active-avatar safe-mode permission gate, real forced pause, callback/world failure, replacement-adapter rejection
and an in-process
two-runtime wait-only barrier.

These are not production two-player routing. The single-player production server now owns an authoritative
simulation-thread session directory and two-stage auth/resume path; protocol minor `1` carries the client's last
accepted generation, exact lost-response replay is retired only after an exact-tuple application confirmation reaches
the simulation-thread directory, and stale tuples cannot replace a newer binding. Phase 3 Gate 1 is closed by source
`cbd19b48d652be735a3c83fe841d2d7c831aecba`: the pure `multiplayer_selected_root_lifecycle` contract keeps transport,
barrier, runtime and dormant states independent; directory/runtime APIs provide exact offline/reactivation,
graceful-release-pending and commit-owned unpublished cleanup; scheduler APIs provide same-generation replay and
disconnected `+1` repair. This is contract/API evidence only. The adapter is still called only by tests;
`do_turn_impl()` still calls the actual legacy bubble directly rather than through `claim_world()`; no production owner
yet composes the lifecycle effects; and fail-stop has no production save/shutdown recovery owner. `players.max` must
remain `1`.
Phase 1/2 hosted platform and Android lifecycle evidence remains valid as recorded in the build baseline and refactor
plan; do not rerun or restate it as evidence for unrelated Phase 3 shared-code changes. The five `do_turn()` trace
labels remain observation points, not safe ownership boundaries, and the isolated `tools/` transport spike is not
production-source portability evidence.

The last hosted-green selector source `804101995c175057856529be24439b0d7e87a49a` has a terminal-green Linux production
gate and a one-time terminal-green CI-contract package matrix recorded in the build baseline. That closes the
workflow/selector milestone only; routine backend-neutral Phase 3 work remains Linux-first.

Current Gate 1 source `cbd19b48d652be735a3c83fe841d2d7c831aecba` has local Linux release/full-multiplayer
and lifecycle ASan/UBSan/LSan evidence plus terminal-green Linux production run `29392575820`; its Windows/Android
portable-spike jobs were precisely skipped. The most recent protocol public-boundary baseline remains run
`29385561653` for source `eb990c4ad9975915336f3acd65431b47d123e842`; it selected only Windows/Android actual-source
package fallback and skipped Linux package. That closes the one-time Tier 2 minor-`1` boundary, not the Phase 3 exit or
a requirement for future internal `.cpp` changes.

Completed Phase 0 gates:

- Local branch: `multiplayer/main`; baseline tag: `multiplayer-upstream-baseline-d84b90d`; fork remote:
  `https://github.com/wsdx233/Cataclysm-DDA-Multiplayer.git`.
- Hosted baseline run `29205262759` produced verified Linux curses, Android arm64 and Windows x64 MSVC artifacts.
  The Windows executable reports exact `+tiles, +sound`; the Android APK is arm64-only and requests `INTERNET`.
- Hosted transport run `29205262750` passed standalone Asio/TCP framing and shutdown contracts on GCC 13,
  Clang 18 and MSVC, plus the Android NDK arm64 compile gate. ADR-0003 is accepted with no embedded TLS:
  plaintext defaults to loopback, trusted-LAN use is explicit, and WAN requires an external authenticated tunnel.
- The stable-avatar bridge, runtime registry, player snapshots, movement/map-shift, mount, vehicle, grab and remote
  control matrix pass 274 release and sanitizer assertions. Full avatar move-swap retains three expected identity
  failures. ADR-0005 is accepted.
- `game::do_turn()` has five observable phase boundaries and a source audit for world-phase active-player getters;
  see `doc/multiplayer/DO_TURN_PHASE_AUDIT.md` for exact evidence.

The active work, in order, is:

1. Keep `players.max = 1` while implementing the closed Gate 1 contract in the production single-root owner. Wire the
   scheduler/adapter into the command loop, put the actual `process_legacy_single_player_bubble_turn()` call behind the
   world claim, stop new turns while root-dormant and resume the same runtime. Map a latched execution fault to typed
   fatal shutdown; if a non-idempotent side effect may already have happened, stop without writing a new canonical
   save. Only failures proven to precede gameplay side effects may use normal save. Close this slice with the
   risk-selected Linux full/sanitizer/process gates, including the headless-server/native-client PTY regression.
   Replace or bypass legacy `execute_turn_player_action()` bookkeeping when the adapter owns an action; wrapping the
   adapter inside the current `do_turn_remote()` bool callback would record the action twice. Move the transport/event/
   admission pump outside that action callback so an initially unbound or dormant server can authenticate/resume
   without first entering `turn_begin`/`player_begin`; dormant must remain in the pump without starting a turn. The new
   owned-turn seam also needs a mandatory pre-world/player-phase-complete hook because sleep, activities or zero moves
   can skip the action callback entirely; use an explicit scheduler terminal transition, not a fabricated command
   result. Scheduler world completion occurs inside the world hook, but lifecycle turn-boundary completion must wait
   until player-end finishes and the owned turn returns. Scene publish, save and the next turn stay after that record.
   Shutdown/save must consult the lifecycle safe-boundary disposition; an open turn is not a normal-save point.
2. Before recording graceful completion, distinguish an ACK actually queued by transport from fallback close;
   `multiplayer_dedicated_server::complete_graceful_disconnect()` currently treats the lobby action as queue success.
   If any external directory/scheduler/runtime/transport effect succeeds but the matching lifecycle record is neither
   applied nor duplicate, immediately latch the lifecycle fault and enter typed fatal shutdown. Refuse a new canonical
   save only when a non-idempotent gameplay/world side effect may have happened or canonical state is otherwise not
   provable; a failure proven to precede such side effects may use the normal save path. Keep each
   directory plan, root admission recipe and enqueue result in one private owner transaction; the recipe is not a
   directory commit receipt.
3. Promote the already-green in-process two-runtime wait-only case into that production owner, then close movement,
   collision, monster/death, field/scent/NPC, tether/group-shift and player-state isolation gates before enabling a
   second client. Do not pull portable characters, multiple save generations or Phase 4 durable restart resume into
   these entry slices.

The Linux curses artifact is still the non-SDL packaging baseline, but the same binary now has an explicitly selected
`--server` runtime path. Do not ship it as a production dedicated-server package until dedicated-server packaging
evidence, operational docs and the later release/operations gates are complete.

## Build Baseline

The fork-specific workflow is [`.github/workflows/multiplayer-baseline.yml`](.github/workflows/multiplayer-baseline.yml). It builds:

- Linux x64 curses tarball.
- Windows x64 MSVC SDL3 Tiles+Sound zip.
- Android arm64 unsigned release APK.
- Android x86_64 debug APK as an emulator/client compile artifact (not a release package).
- Pinned default tileset, soundpack, desktop shaders, and compiled translations.
- Per-artifact provenance, SHA-256, CLI/resource and smoke-test output.

The workflow runs manually and on relevant changes pushed to, or proposed against, `multiplayer/main`. Its current UI
default is `target=all`, but that default is not a validation policy: select `linux`, `windows`, `android` or `all`
explicitly from the completed batch's acceptance target. Automatic runs compare changed paths and select only affected
packages. Ordinary backend-neutral internal gameplay/policy files do not trigger the package baseline; protocol,
public-header and client/platform exceptions are listed in the build baseline. Windows-owned paths select Windows,
Android-owned paths select Android, shared graphical/platform adapters select their affected targets, and shared
artifact/toolchain paths select their required matrix. `Makefile` is Linux-only; root CMake/version generation uses a
targeted Linux configure; public protocol/schema/transport/crypto boundaries temporarily use actual-source Windows/
Android packages. That package execution is a temporary CI implementation detail for Tier 2 portability, not
an instruction to require package or lifecycle acceptance when only the public compile boundary changed.
An unresolvable automatic base/diff fails and requires an explicit manual
target instead of silently running every package. The transport workflow defaults manual and routine automatic runs
to Linux; its Windows/Android jobs compile only the isolated transport spike and run automatically only for that spike
or workflow boundary. It does not create a GitHub Release and does not need production signing secrets.

Pinned build values include:

| Component | Value |
| --- | --- |
| Upstream baseline | `d84b90dd2aee090ca28c8dad5cdf1fab6dea151a` |
| Windows runner | `windows-2022` |
| Linux/Android runner | `ubuntu-24.04` |
| Windows CMake | `3.31.6` |
| vcpkg | `f6672d8e480ccdecddfad3fd1b838ba369ffe6cd` |
| Android JDK | `17` |
| Android platform | `35` |
| Android Build Tools | `34.0.0` |
| Android CMake | `3.22.1` |
| Android NDK | `28.1.13356709` |
| standalone Asio | `1.38.1` / `asio-1-38-1` |
| Asio archive SHA-256 | `2827b229972be80cdb14e5497962fa393d1adf036b5869e2b9c99f644daadacc` |

Do not casually update pinned versions. Change them intentionally, verify all affected platforms, and update the baseline document in the same change.

## Local Environment

Activate the current workspace toolchains before local builds:

```bash
source build-scripts/activate-multiplayer-build-env.sh
```

Default local locations are:

- JDK 17: `~/.local/opt/temurin-17`
- Android SDK: `~/Android/Sdk`
- User-level Linux toolchain: `~/.local/toolchains/cdda-linux`

For the default Linux-first development loop, run:

```bash
./build-scripts/check-multiplayer-build-env.sh linux
```

Use `./build-scripts/check-multiplayer-build-env.sh all` only when a completed toolchain/artifact/release batch or an
explicit multi-platform acceptance target requires every configured toolchain. A phase exit by itself is an evidence
review, not an automatic request for this command.

The local Linux toolchain uses GCC 13, Make, gettext, ncurses, zlib, and bzip2 from the user prefix because system package installation may not be available. The prefix must include matching ncursesw/tinfo runtime libraries as well as development files; the environment gate rejects mixed shared ncurses/static tinfo links. Local `ccache` is optional; CI enables it.

`android/local.properties` is ignored and currently points Gradle at `~/Android/Sdk`. Do not commit machine-specific SDK paths.

## Linux Curses Build

Fast local compile:

```bash
source build-scripts/activate-multiplayer-build-env.sh
make -j"$(nproc)" \
  COMPILER=g++-13 \
  TILES=0 SOUND=0 RELEASE=1 LOCALIZE=0 \
  BACKTRACE=0 PCH=0
./cataclysm --version
```

The repository only commits a translation placeholder, so the fast local build disables localization. The baseline CI builds the package and then injects the pinned official `.mo` files.

For release-like Linux packaging, prefer the CI workflow. A local `make bindist` runs upstream `distclean`, which can remove the tracked empty `lang/mo/.gitignore`; always inspect `git status` afterward and restore that empty placeholder if necessary.

Current local output is normally `cataclysmdda-0.J.tar.gz`. Generated binaries, objects, bindist trees, and archives are ignored and must not be committed.

## Android Build

Environment check:

```bash
source build-scripts/activate-multiplayer-build-env.sh
./build-scripts/check-multiplayer-build-env.sh android
```

Arm64 debug APK:

```bash
cd android
./gradlew \
  -Pj="$(nproc)" \
  -Pabi_arm_32=false \
  -Pabi_arm_64=true \
  -Pabi_x86_32=false \
  -Pabi_x86_64=false \
  -Plocalize=false \
  assembleExperimentalDebug
```

Unsigned arm64 release APK:

```bash
cd android
./gradlew \
  -Pj="$(nproc)" \
  -Pabi_arm_32=false \
  -Pabi_arm_64=true \
  -Pabi_x86_32=false \
  -Pabi_x86_64=false \
  assembleExperimentalRelease
```

Outputs are under:

- `android/app/build/outputs/apk/experimental/debug/`
- `android/app/build/outputs/apk/experimental/release/`

The local debug APK is signed with the Android debug key. The baseline release APK is intentionally unsigned. Production signing and AAB publication belong to the later release/operations phase.

The Android manifest includes `android.permission.INTERNET`; the baseline workflow verifies it in the packaged APK.

## Windows Build

Use Windows 11, Visual Studio 2022, the C++ desktop/game workloads, CMake 3.31.x, MSYS2 gettext/make, and the pinned vcpkg revision.

Check a Windows developer shell with:

```powershell
.\build-scripts\check-multiplayer-build-env.ps1
```

Build and package:

```powershell
msbuild -m `
  -p:Configuration=Release `
  -p:Platform=x64 `
  "-target:Cataclysm-vcpkg-static;JsonFormatter-vcpkg-static;zzip" `
  msvc-full-features\Cataclysm-vcpkg-static.sln

.\build-scripts\windist.ps1 -SDL3
```

MinGW cross-compilation on Linux does not replace native MSVC evidence. Run the hosted `windows-2022` job once a
coherent Windows-owned batch or MSVC-sensitive public boundary is ready for acceptance, and whenever a release or
Windows behavior claim requires fresh evidence. Prefer a lightweight gate that
actually compiles the changed production source; require the full Windows package only when package/runtime behavior
is the acceptance target. Backend-neutral internal shared C++ changes do not require Windows on every iteration.

## Verification Expectations

Use the three verification tiers defined by the refactor plan and mapped to concrete jobs by the build baseline:

1. **Tier 1 — Linux edit loop and slice closure.** During implementation, use incremental Linux builds and the focused
   tests for the changed area. Do not turn every edit into a full-suite gate. When a coherent slice is ready to close,
   add the complete `[multiplayer]` suite, sanitizers and/or a real Linux headless-server/native-client PTY loopback in
   proportion to lifecycle, ownership, protocol and process risk. A Linux curses or SDL client and Linux `--server`
   process are valid functional evidence for backend-neutral shared code.
2. **Tier 2 — completed platform/public-boundary batch.** Run platform evidence once the relevant Windows-owned,
   Android-owned or cross-platform public-boundary batch is complete enough to accept. Wire/schema/version/capability,
   public DTO/serialization layout, compiler-sensitive public headers and shared toolchain/source contracts require
   the smallest targeted gate that actually compiles the changed production source. Shared internal C++ alone does
   not require both packages. A compile-only or cross-compile smoke proves only that boundary and never substitutes
   for package/lifecycle evidence when those are the acceptance target.
3. **Tier 3 — milestone evidence review.** A phase exit reviews whether its claims are covered; it does not
   automatically rerun every platform. The current Linux candidate still runs the phase's required server/client,
   integration and sanitizer gates. For an unchanged platform surface, the review may cite the latest compatible
   evidence commit plus an explicit diff audit, while stating that the current candidate was not compiled or run on
   that platform. Release candidates, pinned toolchain/artifact/signing changes, explicit cross-version compatibility
   claims and new platform behavior claims require fresh evidence on every affected platform.

Platform-owned surfaces include `msvc-full-features/`, Windows batch/PowerShell/package code, Win32-only filesystem/
process/socket/ACL or SDL behavior, plus Android Gradle/CMake/manifest/Java/JNI/ABI/resource/touch/lifecycle code.
Portable `src/multiplayer_*` policy/rule code is not platform-owned unless it crosses one of those boundaries or adds
platform conditionals. Record the selected tier, commands, results and intentionally unrun platforms in `STATUS.md`;
"not run by policy" is not a blocker, but it cannot be presented as platform evidence.

Do not run `check-multiplayer-build-env.sh all`, a full baseline matrix, Windows package or Android APK by habit. Start
with the incremental Linux edit loop, close the slice with risk-selected Linux gates, and escalate only for a completed
platform/public-boundary batch or a milestone claim that requires fresh evidence. A job that did not compile or run
the changed production source is not evidence for that change; in particular, the isolated Windows/Android
transport-spike jobs do not prove scheduler, phase-adapter or other production `src/multiplayer_*` portability.

The baseline changed-path selector is an optimization, not an authority oracle. When a platform-owned file is added
or renamed, update both push/pull-request path lists and the selector mapping in the same change. If a generic path
adds or modifies a platform conditional that path matching cannot detect, run the affected Tier 2 gate explicitly and either
extend the selector or record why the case remains manual.

The workflow does not batch coherent platform work automatically: each matching push/PR may start a package run.
Accumulate intermediate platform/public-boundary commits locally or on a topic branch and push the frozen acceptance
candidate when practical; do not use repeated pushes to `multiplayer/main` as the edit loop. A future lightweight
production portability target or explicit batch trigger may replace the current package fallback.

`multiplayer-transport-spike.yml target=all` means Linux production plus Windows/Android isolated spike probes; it is
not a product/package matrix. Use the baseline workflow only when the selected platform/public/release acceptance
target actually requires package evidence.

For changed Bash/workflow/toolchain code, still run the relevant syntax/lint checks. Verify artifact ABI, provenance
and SHA-256 whenever an artifact is actually part of the selected tier. Multiplayer simulation tests continue to
scale with risk; the full-swap tests remain a negative identity comparison.

## Engineering Constraints

- Keep diffs additive and narrowly scoped. Avoid unrelated formatting, mass renames, directory moves, and broad global-access rewrites.
- Preserve upstream single-player behavior and old-save compatibility. Multiplayer and single-player should share rule executors.
- World mutation may only occur on the simulation thread. Never call game-rule methods from a per-player network thread.
- Lobby auth/resume may validate transport data on a network thread, but accepted identity/generation/admission must
  wait for authoritative simulation-thread session-directory commit; follow ADR-0010.
- A resume request must declare the client's last accepted generation and be checked against the token mirror and
  runtime. A retry of a lost accepted response may replay only the immediately committed generation with the same
  revision/sequence fingerprint; it must not increment twice. The first valid application frame starts exact-tuple
  confirmation, but the replay permission is not considered consumed authoritatively until the simulation-thread
  directory accepts that confirmation and the server explicitly acknowledges it back to the lobby mirror. Failure
  paths must fail-stop or remain fail-closed without advancing the generation again.
- The directory/runtime generation is authoritative. A lobby token mirror may lag exactly one generation only after an
  accepted resume commit whose response could not be enqueued; the next matching directory replay must repair it.
  Fresh authentication has no published token in that case and therefore gets a new admission, not same-generation
  replay.
- A fresh token not yet confirmed by any client application frame must not strand player capacity after disconnect.
  Terminal `session_expired` rejection releases an inactive token record; active/pending conflicts preserve it and use
  a retryable non-terminal rejection. Typed rejection bytes and close must use one ordered transport command, with a
  queue-full close fallback.
- Network/compression workers must not read live game objects. Build immutable command and snapshot DTOs at defined safe points.
- Server command execution must not call blocking UI such as `query_yn()`, `uilist::query()`, inventory selectors, SDL, curses, or ImGui.
- Keep network code independent from concrete UI backends, and keep game rules independent from sockets.
- Do not spread multiplayer conditionals throughout gameplay code. Prefer router, context, guard, adapter, and executor boundaries.
- Keep multiplayer source files under the planned `multiplayer_` prefix until the module and build source lists stabilize.
- Treat protocol version, server-state schema, portable-character schema, and savegame version as separate compatibility axes.
- If a scheduler/phase fault may follow a non-idempotent gameplay side effect, stop simulation and do not write a new
  canonical save without a journal/rollback proof.
- Update ADRs, the action coverage matrix, known limitations, migration notes, and relevant plan sections with architectural changes.

## Prohibited Shortcuts

Do not:

- Point multiple CDDA processes at one save directory.
- Implement cross-platform deterministic lockstep.
- Upload or deserialize an entire client `.sav` as a server character.
- Send complete worlds or internal submaps to clients.
- Trust client-provided positions, damage, inventory results, moves, visibility, or RNG outcomes.
- Use pointers, container indexes, or unstable integer action IDs as network identities.
- Add locks around simulation objects and execute game rules concurrently.
- Block the server simulation stack while waiting for a remote menu response.
- Rewrite every UI or every `get_avatar()` call before the vertical slice proves the boundary.
- Pull multi-bubble simulation, NAT traversal, public accounts/lobbies, automatic mod download, or MMO scale into v1.
- Design custom cryptography instead of TLS 1.3 or an explicit external VPN/tunnel policy.

## Git And Generated Files

- Current branch: `multiplayer/main`.
- Current baseline tag: `multiplayer-upstream-baseline-d84b90d`.
- `origin` fetches from and pushes to `https://github.com/wsdx233/Cataclysm-DDA-Multiplayer.git`.
- `upstream` fetches from `https://github.com/cleverraven/cataclysm-dda.git`; its push URL is disabled.
- Never enable pushes to the CleverRaven upstream remote.
- The worktree may contain user-authored and generated changes. Do not discard or reset changes you did not create.

Common generated/ignored paths include:

- `VERSION.txt`, `src/version.h`
- `obj/`, `bindist/`, `cataclysm`, `cataclysmdda-*.tar.gz`
- `android/.gradle/`, `android/app/.cxx/`, `android/app/build/`, `android/app/libs/`
- `android/local.properties`
- generated `lang/mo/*`, except the tracked empty `lang/mo/.gitignore`
- generated shader binaries and formatter/build helper outputs

Do not commit local APKs, archives, SDK paths, downloaded AARs, object files, or generated translation trees.
