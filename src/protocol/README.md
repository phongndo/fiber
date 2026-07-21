# Protocol

Bounded local messages between Fiber clients and the daemon. This component owns schemas, limits,
encoding, incremental decoding, and the current handshake values.

It does not open sockets, discover workspaces, dispatch core commands, or perform terminal I/O. Every
length, enum, and identifier from the wire must be validated before entering the core. Parsers must
support fragmented input and reject oversized messages without unbounded allocation.

The present one-client-per-workspace format is intentionally unversioned. Add version and capability
negotiation as one tested generalized-protocol change; see `docs/protocol.md`.
