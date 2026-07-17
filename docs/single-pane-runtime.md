# Single-pane runtime

## Status

Fiber's first end-to-end vertical slice has been migrated out of the former monolithic
`single_pane.cpp`. The behavior remains intentionally single-pane, but its ownership now follows the
production component architecture:

```text
apps/fiber/main.cpp
        |
        v
     app/run
      /   \
 client   daemon
             \
              core engine -> terminal adapter -> libghostty-vt
                   |
                   +------> renderer / platform / protocol
```

There is no temporary demo component or `app -> demo` dependency. New multi-pane work must build on
the core, client, daemon, protocol, and render boundaries rather than recreate a vertical-slice
monolith.

## Current ownership

### Application — `src/app/`

Parses commands and selects client or daemon operations. It contains the diagnostic `fiber demo`
command but no session runtime logic. `apps/fiber/main.cpp` only delegates to `fiber::app::run`.

### Client — `src/client/`

Owns the attached process side: raw-terminal setup/restoration, `SIGWINCH` observation, prefix
parsing, input/resize packet emission, daemon output forwarding, and detach behavior. It holds no
pane or terminal-emulator state.

### Daemon — `src/daemon/`

Owns session names, per-user socket paths, discovery, locks, stale-socket validation, daemonization,
listener lifetime, control commands, and cleanup. It creates and owns the listener, lends the
descriptor to the core engine, and releases the endpoint when the engine finishes its reactor and
before the engine reaps the child.

### Core — `src/core/`

Owns the running pane: child process, PTY, `vt::Terminal`, attached daemon-side client descriptor,
protocol-message application, PTY drain budgets, frame scheduling, backpressure state, and child
shutdown. The reactor borrows the daemon listener and remains the sole owner of mutable pane state.

### Supporting components

- `src/platform/`: descriptor I/O, PTY/process operations, and terminal mode;
- `src/protocol/`: bounded packet encoding, prefix parsing, and incremental decoding;
- `src/render/`: retained frame buffers and partial nonblocking client writes;
- `src/terminal/`: the sole private `libghostty-vt` adapter.

## Supported behavior

The runtime currently provides:

- up to 64 validated named sessions;
- one shell, PTY, and canonical Ghostty terminal per session;
- one attached client plus independent list/kill control connections;
- detach with `C-b d` and literal prefix with `C-b C-b`;
- terminal resize forwarding;
- bounded protocol and PTY read batches;
- terminal-generated PTY responses;
- dirty-row rendering and retained physical client state;
- a 2 ms frame-coalescing deadline;
- partial nonblocking live-frame writes;
- full visible-state reconstruction on reattach;
- deterministic child, descriptor, socket, and lock cleanup.

## Current limitations

The architecture is migrated, but the product remains a single-pane implementation:

- one daemon process represents one session;
- sessions have no windows or split layout tree;
- only one client may attach at a time;
- the local protocol has no version or capability negotiation;
- listener acceptance and initial attach setup still use the vertical slice's simple policy;
- extension commands/events and configurable key tables are not integrated;
- the renderer does not yet compose multiple pane surfaces.

These are feature and runtime-hardening tasks, not reasons to reorganize the source tree again.

## Build-out sequence

1. Introduce dense generational stores for sessions, windows, panes, and clients.
2. Represent all topology changes as typed core commands.
3. Generalize the borrowed listener integration to multiple pending and attached clients.
4. Replace the single-pane poll set with the descriptor registry/reactor abstraction.
5. Add split-tree layout and per-client viewport state.
6. Generalize rendering from one terminal surface to composed pane rectangles.
7. Version the protocol and add capabilities, request IDs, and typed errors.
8. Add deferred, budgeted extension commands and immutable events.
9. Add replay traces, capacity tests, fuzzing, and end-to-end latency benchmarks.

## Rules for contributors and agents

- Never move client raw-terminal state into the daemon or core.
- Never move socket naming, locks, or daemonization into the core.
- Never expose Ghostty headers outside `src/terminal/terminal.cpp`.
- Do not let client or extension work block PTY progress.
- Preserve bounds when generalizing a single object into an arena.
- Add state transitions through typed commands instead of direct cross-component mutation.
- Keep structural refactors separate from new mux behavior.
- Update this document when ownership or a runtime invariant changes.
