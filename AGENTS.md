# CDDA Multiplayer Fork Agent Guide

## Scope And Sources Of Truth

This file applies to the entire repository.

Before changing multiplayer architecture or build/release behavior, read:

- [`doc/MULTIPLAYER_REFACTOR_PLAN.md`](doc/MULTIPLAYER_REFACTOR_PLAN.md): product scope, architecture, phases, risks, tests, and prohibited shortcuts.
- [`doc/MULTIPLAYER_BUILD_BASELINE.md`](doc/MULTIPLAYER_BUILD_BASELINE.md): pinned toolchains, CI artifacts, local setup, and verified build status.

The refactor plan is the source of truth for fork-specific architecture. Existing upstream CDDA patterns remain the default for code style and unchanged game behavior. If code investigation invalidates a plan assumption, record the decision in an ADR and update the plan instead of silently diverging.

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
7. Existing `game::u` is retained as the active-avatar execution slot. Phase 0 must decide between full avatar slot swapping and the sidecar fallback.
8. Input parsing and rule execution must be separated. Single-player and multiplayer should call the same command executors.
9. Clients must not upload a complete `.sav`. Character transfer uses a versioned portable package with ID remapping and world-reference cleanup.
10. Headless execution must not initialize or block on SDL, curses UI, ImGui, sound, popups, or local menus.

See the plan sections on authority, threading, state scope, scheduling, action/UI separation, protocol, headless startup, security, and testing for the full requirements.

## Current Phase

The project is in **Phase 0: baseline, ADRs, and feasibility validation**. Do not treat the repository as already being in protocol production work.

Completed baseline work:

- Local branch: `multiplayer/main`.
- Baseline tag: `multiplayer-upstream-baseline-d84b90d`.
- Fork remote: `https://github.com/wsdx233/Cataclysm-DDA-Multiplayer.git`.
- Linux curses package builds and passes archive, `--version`, and dynamic-library smoke checks.
- Android arm64 debug and unsigned release APKs build and pass ZIP, ABI, badging, and signing-state checks.
- The Windows MSVC build is configured in CI but still requires its first run on a `windows-2022` runner.
- Full compiled translations are taken from the hash-pinned official baseline release package, without requiring a fork Transifex secret.

The next Phase 0 work, in order, is:

1. Run the complete baseline workflow on the fork, especially Windows MSVC.
2. Add ADRs for authority, time model, transport/TLS, protocol, player bridge, rendering, saves, character policy, and bubble policy.
3. Build the avatar slot-swap versus sidecar test spike under ASan/UBSan.
4. Build the standalone Asio/TCP transport spike on Linux, MSVC, and Android NDK; validate the TLS packaging decision.
5. Enter Phase 1 only after the player-bridge go/no-go decision and three-platform transport compile gate pass.

Linux curses is only the current non-SDL build baseline. It is not a multiplayer server and must not be described or shipped as one.

## Build Baseline

The fork-specific workflow is [`.github/workflows/multiplayer-baseline.yml`](.github/workflows/multiplayer-baseline.yml). It builds:

- Linux x64 curses tarball.
- Windows x64 MSVC SDL3 Tiles+Sound zip.
- Android arm64 unsigned release APK.
- Pinned default tileset, soundpack, desktop shaders, and compiled translations.
- Per-artifact provenance, SHA-256, and smoke-test output.

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

The local Linux toolchain uses GCC 13, Make, gettext, ncurses, zlib, and bzip2 from the user prefix because system package installation may not be available. Local `ccache` is optional; CI enables it.

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

The original baseline manifest does not yet include `android.permission.INTERNET`; add it with the Android transport spike, not as an unrelated baseline change.

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

For multiplayer simulation changes, scale tests with risk. Phase 0 requires sanitizer-backed avatar swap/sidecar invariants. Later phases require loopback server/client tests, two-player conflict tests, reconnect/idempotency tests, save round trips, visibility leak tests, fuzzing, and soak tests as specified in the plan.

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
