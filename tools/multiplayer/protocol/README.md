# Multiplayer protocol schema

`multiplayer_protocol.fbs` is the versioned semantic wire schema accepted by ADR-0004. The generated C++
header is committed as `src/third-party/multiplayer_protocol_generated.h`; do not edit it manually.

Generation is pinned to FlatBuffers 1.12.0, matching the runtime already vendored by CDDA:

- Tag: `v1.12.0`
- Commit: `6df40a2471737b27271bdd9b900ab5f3aec746c7`
- Archive SHA-256: `62f2223fb9181d1d6338451375628975775f7522185266cd5296571ac152bc45`

Run:

```bash
FLATC=/path/to/flatc tools/multiplayer/protocol/generate.sh
FLATC=/path/to/flatc tools/multiplayer/protocol/generate.sh --check
```

Schema changes must be append-only within a protocol major version. Envelope protocol version, server-state
schema, portable-character schema, player snapshot schema, and savegame version remain separate axes.
