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

Phase 0 closed on 2026-07-12 and Phase 2 closed on 2026-07-14. The project is now in **Phase 3,
Gate 3: two-runtime owner and shared-rule matrix**. Gate 1 source
`cbd19b48d652be735a3c83fe841d2d7c831aecba` closed the selected-root lifecycle contract; Gate 2 source
`2e9236c7bf91782ad3f15daa5d8aa0e3929f355a` closed the production single-root lifecycle and caused ADR-0010/0011 to
be accepted.

The production server now uses `multiplayer_single_root_owner` as the simulation-thread owner of the authoritative
session directory, scheduler, lifecycle and phase adapter. Semantic command and forced wait execution go through the
adapter; the actual legacy bubble runs only behind an exact world claim; lifecycle completion waits for player-end;
root-dormant stops new turns and same-runtime resume reactivates it. Admission publish and graceful transport results
are typed. Open turns or uncertain post-side-effect faults fail-stop without writing a new canonical save. ADR-0011's
outer/active-player-input pumps allow initial/dormant auth and open-barrier resume while replaying only immutable
completed-scene payloads.

This is still single-root production routing, not two-player support. `players.max` must remain `1`. The five
`do_turn()` trace labels remain observation points rather than ownership boundaries. Production game-over/death still
maps to fatal/no-save until the Gate 3 death policy is closed.

Gate 3 source `2eccb92087991966423c18b63f6ef707462b1428` closes only the first **inner multi-runtime barrier owner**
slice. `multiplayer_multi_runtime_barrier_owner` owns a persistent fair scheduler, validates and pins an immutable
runtime/avatar roster, rejects same-key owner replacement before action/wait/world work, and holds an explicit
`player_end_pending` boundary until an exact process-local receipt is recorded. It does not own transport, session
directory, selected-root changes, actual legacy bubble execution, outer lifecycle/save proof or production command
routing.

Gate 3 source `3204f8f45606a20ea6ab0892369b9806f89c4353` closes the second inner rule slice. A thin
`multiplayer_multi_runtime_command_router` routes only the exact current barrier participant through the existing
basic command executor, maps the authoritative execution result and target moves to a scheduler disposition, and
fail-closes movement to the audited same-z adjacent empty-ground subset. Owner-level tests cover
`move -> move -> wait -> wait`, target-only position/moves/action bookkeeping, tracker ownership and root-context
restoration. The directly reached legacy walk seams now resolve `active_avatar()` and remote movement cannot enter a
dangerous-terrain prompt. This still does not provide production transport, revision/dedup, session ownership, actual
bubble execution or two-client routing.

Gate 3 source `84d8ca056bf72ed9776890f7ca4212a3bfdf7c2e` closes the third inner rule slice. The registry can resolve an
exact-position human occupant other than the mover, and the router returns an internal typed `blocked_by_player` result
with the blocker's exact participant key before entering legacy movement. The authoritative result is rejected with
zero moves, zero action bookkeeping and no scheduler advance; it never implies swap or PvP attack. Owner-level tests
cover symmetric adjacent blocking and two-turn contention for one empty tile, including rotation of the ordering-first
winner and exact world/player-end closure. The blocker key is internal authority data that a future outer wire owner
must visibility-filter. This remains an inner contract without a production caller, actual bubble or two-client route.

The first owner slice has local Linux focused/full-multiplayer and ASan/UBSan/LSan evidence plus a Linux curses client
version smoke. Slice 2 has Linux focused/full-multiplayer, targeted sanitizer and native curses PTY regression evidence
recorded precisely in `STATUS.md`; the PTY protects the unchanged single-root route, not a two-runtime production
caller. Slice 3 has Linux focused/full-multiplayer, targeted sanitizer and format evidence recorded in
`STATUS.md`; it has no production caller, so no process/PTY rerun is required. Gate 2 retains its open-turn save-hash
evidence. These Gate 3 diffs do not change wire/schema/version, platform conditionals, shared build lists, pinned
toolchains or Windows/Android-owned code, so Windows/Android remain
intentionally unrun. The latest compatible
public-boundary evidence remains run `29385561653` for protocol-minor-1 source
`eb990c4ad9975915336f3acd65431b47d123e842`; it is not current Gate 3 platform evidence. Routine Phase 3 work remains
Linux-first under the validation policy below.

The active work, in order, is:

1. Keep `players.max = 1` and keep `multiplayer_single_root_owner::create()` fixed at exactly one player. Treat source
   `2eccb92087991966423c18b63f6ef707462b1428`, `3204f8f45606a20ea6ab0892369b9806f89c4353` and
   `84d8ca056bf72ed9776890f7ca4212a3bfdf7c2e` as inner barrier/rule contracts only; do not wire them directly into the
   production single-root loop or mistake their lambda world callback for actual bubble evidence.
2. Split the current work into 4A monster target/attack and 4B death/game-over safe-boundary policy. Then close
   field/scent/NPC; tether/group shift; and dedicated messages/safe-mode/stats/player-scoped cache isolation. Preserve
   one shared bubble and single simulation thread. Each slice uses Linux incremental/focused tests; authority/lifecycle
   risk selects full `[multiplayer]` or sanitizer, and a process smoke is added only after the changed production route
   is actually reachable.
3. After owner/rule gates are green, expose two connections only behind an integration/process-test switch and run a
   Linux two-client smoke/soak. Do not publish `players.max > 1` until that evidence exists. Portable characters,
   durable restart resume, multiple save generations and Phase 4 replica/rendering remain out of scope.

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

The workflow runs manually and on relevant changes pushed to, or proposed against, `multiplayer/main`. Manual runs
default to `target=linux`; select `windows`, `android` or `all` only when a completed platform/public/release batch has
that acceptance target. Automatic runs compare changed paths and select only affected packages. Ordinary backend-
neutral internal gameplay/policy files do not trigger the package baseline; protocol,
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
   tests for the changed area. Do not turn every edit into a full-suite gate. Classify closure by production
   reachability: an internal/test-only contract may close with Linux compile/focused tests plus risk-selected full or
   sanitizer coverage. For reachable production code, `client` uses a Linux native-client smoke, `server` uses a Linux
   headless process, and only `end-to-end` requires both sides in a PTY/loopback. Use a Linux SDL client only for
   graphical/rendering behavior; otherwise the curses client is the default representative client.
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

For every closure, record the changed-source reachability in `STATUS.md` as `test-only`, `client`, `server` or
`end-to-end`. A skipped process gate is valid when the production binary cannot reach the changed code; a process smoke
that never reaches it is not evidence. Platform gates remain claim-driven: compile-only, package and native lifecycle
results are not interchangeable.

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
