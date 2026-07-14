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

Phase 0 closed with recorded evidence on 2026-07-12, and Phase 2 closed with recorded evidence on 2026-07-14. The
project is now in **Phase 3: second-player/shared-scheduler investigation**; no second-player scheduler has been
implemented yet. Phase 1 and Phase 2 local, hosted and Android lifecycle gates are green. The Phase 2 client
implements production transport/state, desktop/Android connection UI, semantic
wait/move input, bounded full-scene fitting, heartbeat/manual reconnect, ordered clean session release and local
remote-scene rendering. The final transport fix also keeps a closed logical connection's admission slot until its
terminal event is consumed, so reset churn cannot crowd terminal events out of the bounded queue. Commit
`6403a949fb537be14ec4d5757f295adbf5c5f99b` has a green hosted platform gate in baseline run `29303564150` and
transport/protocol run `29303564152`. A KVM-backed API 35 x86_64 run now has green local evidence for clean launch,
auth/render, one controlled wait, one controlled move, Activity pause/resume, forced network disconnect/reconnect and
clean save/shutdown. That run exposed and then verified the fix for a cross-backend handshake bug: commit
`e078eb6aef25a9cc72eea45793114c931a52b896` uses one backend-neutral 40-character Git SHA, optionally suffixed
`-dirty`, as the multiplayer build ID instead of the display `VERSION`. Its baseline run `29328086046` and
transport/protocol run `29328086326` are terminal `success`, including Linux, Windows MSVC and Android evidence. A
follow-up read-only audit found stale-CMake-generation, Make dirty-state error handling and Windows assertion-case
gaps; pushed commit `86336ea847bea45f727fd97d74a811a32712518c` fixes them. Its baseline run `29330811779`
and transport/protocol run `29330811746` are terminal `success`; this closes Phase 2. The Android KVM lifecycle
evidence belongs exactly to `e078eb6`; do not imply it was rerun for `86336ea`. Phase 3 begins with source/ownership
investigation, not a claim that the two-player scheduler already exists. Do not describe the isolated `tools/`
transport spike as production networking.

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
  see `doc/multiplayer/DO_TURN_PHASE_AUDIT.md` and `STATUS.md` for exact evidence.

The active work, in order, is:

1. Begin ADR-0002's Phase 3 investigation with the ordered `do_turn`/active-player/monster ownership source audit
   recorded in `STATUS.md`; document conflicts before expanding implementation.
2. Design the smallest second-player registry/context/scheduler slice consistent with the accepted ADRs, then add
   conflict, fairness, tether and player-state isolation tests before enabling `players.max > 1`.
3. Keep the Phase 2 server/client slice hardened, but do not pull portable characters, multiple save generations or
   Phase 4 durable process-restart resume into the scheduler entry slice.

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

The workflow runs manually and on relevant changes pushed to, or proposed against, `multiplayer/main`. It does not create a GitHub Release and does not need production signing secrets.

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

Run the environment gate with:

```bash
./build-scripts/check-multiplayer-build-env.sh all
```

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

MinGW cross-compilation on Linux does not replace the required MSVC build. Validate Windows changes on the hosted `windows-2022` job.

## Verification Expectations

For build-script, workflow, or toolchain changes:

1. Run `./build-scripts/check-multiplayer-build-env.sh all` where applicable.
2. Run `bash -n` and ShellCheck on changed Bash scripts.
3. Run `actionlint` on `.github/workflows/multiplayer-baseline.yml`.
4. Run the relevant local Linux or Android build and its package smoke checks.
5. Run the hosted Windows job for MSVC-specific or shared C++ changes before considering the baseline green.
6. Verify generated artifacts contain only the requested ABI/platform and include build provenance plus SHA-256.

For multiplayer simulation changes, scale tests with risk. Phase 0 requires sanitizer-backed stable-avatar context, persistent-reference, and player-runtime invariants; the full-swap tests remain a negative comparison. Later phases require loopback server/client tests, two-player conflict tests, reconnect/idempotency tests, save round trips, visibility leak tests, fuzzing, and soak tests as specified in the plan.

## Engineering Constraints

- Keep diffs additive and narrowly scoped. Avoid unrelated formatting, mass renames, directory moves, and broad global-access rewrites.
- Preserve upstream single-player behavior and old-save compatibility. Multiplayer and single-player should share rule executors.
- World mutation may only occur on the simulation thread. Never call game-rule methods from a per-player network thread.
- Network/compression workers must not read live game objects. Build immutable command and snapshot DTOs at defined safe points.
- Server command execution must not call blocking UI such as `query_yn()`, `uilist::query()`, inventory selectors, SDL, curses, or ImGui.
- Keep network code independent from concrete UI backends, and keep game rules independent from sockets.
- Do not spread multiplayer conditionals throughout gameplay code. Prefer router, context, guard, adapter, and executor boundaries.
- Keep multiplayer source files under the planned `multiplayer_` prefix until the module and build source lists stabilize.
- Treat protocol version, server-state schema, portable-character schema, and savegame version as separate compatibility axes.
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
