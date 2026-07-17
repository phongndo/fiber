# Fiber local protocol

## Status and scope

The current protocol is the bounded local wire format used by the single-pane runtime. Its
implementation is isolated in `src/protocol/single_pane.*` so framing can be tested independently of
sockets and terminal state. It is not yet the final multi-pane protocol and currently has no version
negotiation; incompatible changes must therefore remain coordinated between the daemon and client.

All integers are unsigned big-endian. Message type values are one ASCII byte for diagnostics only;
they must be treated as binary enum values, not text.

## Control connection

A newly accepted connection starts with exactly one control command:

| Command | Byte | Following bytes | Meaning |
| --- | ---: | --- | --- |
| attach | `A` | 2-byte columns, 2-byte rows | Request a streaming attachment |
| list | `L` | none | Return a bounded human-readable listing and close |
| kill | `K` | none | Request daemon shutdown, return confirmation, and close |

Attach returns one byte:

| Response | Byte | Meaning |
| --- | ---: | --- |
| attached | `Y` | Connection has become the streaming client |
| busy | `B` | This session already has an attached client |

After `Y`, the daemon sends a complete reconstructed terminal frame before switching the connection
to nonblocking live operation.

## Attached-client stream

Only the client sends framed messages. Daemon-to-client traffic is already encoded outer-terminal
bytes and is deliberately unframed in the current protocol.

### Input

```text
+--------+----------------+-------------------+
| 'I'    | length: u16be  | length input bytes|
+--------+----------------+-------------------+
   1 B          2 B             0..8192 B
```

The client prefix parser may emit at most twice the terminal read batch. The decoder rejects larger
lengths before exposing a message. Input bytes are normalized and encoded through the pane's
terminal adapter rather than blindly forwarding recognized control/navigation sequences.

### Resize

```text
+--------+----------------+-------------+
| 'R'    | columns: u16be | rows: u16be |
+--------+----------------+-------------+
   1 B          2 B            2 B
```

The runtime clamps dimensions to its configured hard limits, resizes the PTY, and then resizes the
canonical terminal state. Both operations must succeed.

### Detach

```text
+--------+
| 'D'    |
+--------+
   1 B
```

Detach closes only the attached connection. It does not terminate the shell or session.

## Decoder contract

`protocol::ClientDecoder` owns a fixed 16 KiB buffer and supports arbitrary stream fragmentation and
coalescing. The caller follows this sequence:

1. obtain `writable_bytes()`;
2. receive at most that span's size;
3. call `commit(received)`;
4. call `next()` until it reports incomplete input;
5. process each borrowed message synchronously; and
6. call `consume()` before asking for another message or receiving more bytes.

An input span returned in `ClientMessage` borrows decoder storage and becomes invalid on `consume()`
or `reset()`. It must never be retained in core state or passed to deferred extension work.

Unknown types, oversized lengths, and buffer exhaustion are terminal protocol errors for that
connection. They never partially mutate mux state.

## Prefix parser

The attached client currently recognizes a fixed `C-b` prefix:

- `C-b d` emits a detach message;
- `C-b C-b` emits one literal `C-b` input byte;
- `C-b x` forwards literal `C-b` followed by `x` for any other byte `x`.

The eventual key-table system will replace this fixed policy. The parser remains bounded and outside
terminal VT parsing.

## Evolution requirements

Before the protocol supports multiple clients or independent release versions, add:

- a magic value and protocol version;
- explicit message lengths for every direction;
- request IDs for control commands;
- capability negotiation;
- typed error responses;
- stable entity-ID encoding;
- bounded frame/output message semantics; and
- peer-credential validation at the transport boundary.

Introduce these fields together through the generalized core protocol, with golden encoding tests,
fragmented-input tests, malformed-input tests, and fuzz coverage.
