# Multiplayer development tools

These tools validate the fork's multiplayer boundaries; they are not end-user clients or release artifacts.

## Headless process smoke client

`headless_client_smoke.cpp` is a small TCP client used by
`.github/workflows/multiplayer-transport-spike.yml` against the real `cataclysm --server` process. It verifies:

- incompatible hello rejection and compatible hello acceptance;
- bearer-token authentication, the initial full visible scene, and explicit resync;
- one server-authoritative semantic `wait` command;
- action and post-world scene revisions;
- disconnect/resume with exact generation advancement, followed by idempotent command replay;
- disconnect on reuse of a cached sequence with different command bytes.

It deliberately discovers the running server's compatibility values from a rejected probe hello. That behavior is for an integration smoke only and is not a model for a production graphical client.

The CI workflow compiles it directly with the production protocol, transport, and crypto sources. Never print or pass the bearer token as a command-line value; the tool accepts a token **file path** and does not log its contents.

## Production network-client UI smoke

`network_client_ui_smoke.py` starts the real `cataclysm --connect` path in a private PTY and relays it through a bounded test proxy to an already-running dedicated server. It supplies local wait/move/reconnect/quit input, drops the first connection after forwarding the wait command, and verifies resume plus exactly-once replay before a clean UI exit. The transcript contains terminal control sequences and is retained only as a CI diagnostic artifact; the event log contains no bearer token.

This is a Linux curses rendering fallback for deterministic process automation. It exercises the same production client transport/state machine, semantic input boundary, and remote-scene window used by graphical builds. It is sufficient daily evidence for backend-neutral behavior; it does not replace hosted MSVC or Android package/runtime evidence when a Tier 2 or Tier 3 boundary specifically requires that platform.

## Protocol schema

See [`protocol/README.md`](protocol/README.md) for the pinned FlatBuffers generator and schema regeneration contract.

## Transport spike

See [`transport_spike/README.md`](transport_spike/README.md) for the standalone Asio compile/loopback spike. Production server integration lives under the `src/multiplayer_*` boundary; do not expand the spike into a second transport implementation.
