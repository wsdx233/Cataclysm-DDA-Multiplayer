# Multiplayer development tools

These tools validate the fork's multiplayer boundaries; they are not end-user clients or release artifacts.

## Headless process smoke client

`headless_client_smoke.cpp` is a small TCP client used by
`.github/workflows/multiplayer-transport-spike.yml` against the real `cataclysm --server` process. It verifies:

- incompatible hello rejection and compatible hello acceptance;
- bearer-token authentication, the initial full visible scene, and explicit resync;
- one server-authoritative semantic `wait` command;
- action and post-world scene revisions;
- disconnect/resume and idempotent replay;
- disconnect on reuse of a cached sequence with different command bytes.

It deliberately discovers the running server's compatibility values from a rejected probe hello. That behavior is for an integration smoke only and is not a model for a production graphical client.

The CI workflow compiles it directly with the production protocol, transport, and crypto sources. Never print or pass the bearer token as a command-line value; the tool accepts a token **file path** and does not log its contents.

## Protocol schema

See [`protocol/README.md`](protocol/README.md) for the pinned FlatBuffers generator and schema regeneration contract.

## Transport spike

See [`transport_spike/README.md`](transport_spike/README.md) for the standalone Asio compile/loopback spike. Production server integration lives under the `src/multiplayer_*` boundary; do not expand the spike into a second transport implementation.
