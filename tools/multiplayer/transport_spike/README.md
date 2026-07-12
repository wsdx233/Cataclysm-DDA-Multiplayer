# Multiplayer Transport Spike

This is the disposable Phase 0 transport feasibility program. It is not linked into any game,
client, or server target and must not become the production protocol implementation by accretion.

The spike pins standalone Asio 1.38.1 (`asio-1-38-1`, upstream commit
`dfd7b3e3145bac5d0e91a99fde69c6ae1442f971`) from the upstream tag archive:

- URL: `https://github.com/chriskohlhoff/asio/archive/refs/tags/asio-1-38-1.tar.gz`
- SHA-256: `2827b229972be80cdb14e5497962fa393d1adf036b5869e2b9c99f644daadacc`
- License: Boost Software License 1.0, included in the downloaded source archive

The executable verifies a four-byte big-endian length frame, a hard 1 MiB frame limit, a bounded
thread-safe DTO queue, loopback-only binding, frame fragmentation and coalescing, peer half-close,
accept cancellation, queue saturation, and ordered shutdown.

## Linux

```bash
cmake -S tools/multiplayer/transport_spike -B build/transport-spike-gcc \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_CXX_COMPILER=g++-13
cmake --build build/transport-spike-gcc --parallel
ctest --test-dir build/transport-spike-gcc --output-on-failure
```

Repeat with `-DCMAKE_CXX_COMPILER=clang++-18` for the Clang gate.

## Windows

```powershell
cmake -S tools/multiplayer/transport_spike -B build/transport-spike-msvc `
  -G "Visual Studio 17 2022" -A x64
cmake --build build/transport-spike-msvc --config Release --parallel
ctest --test-dir build/transport-spike-msvc -C Release --output-on-failure
```

## Android arm64

```bash
cmake -S tools/multiplayer/transport_spike -B build/transport-spike-android-arm64 \
  -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_TOOLCHAIN_FILE="$ANDROID_HOME/ndk/28.1.13356709/build/cmake/android.toolchain.cmake" \
  -DANDROID_ABI=arm64-v8a \
  -DANDROID_PLATFORM=android-35 \
  -DANDROID_STL=c++_static
cmake --build build/transport-spike-android-arm64 --parallel
```

The Android cross-build is a compile gate; the loopback behavior runs on Linux and Windows.

## Security Boundary

No embedded TLS backend is selected by this spike. Until a later backend passes TLS 1.3 handshake,
certificate verification, Android/MSVC/Linux packaging, and update-policy gates, production defaults
must bind loopback. Explicit plaintext LAN mode is administrator-only. WAN use requires an external
authenticated encrypted tunnel such as WireGuard or Tailscale. Plaintext WAN listening is unsupported.
