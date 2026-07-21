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
command but no workspace runtime logic. `apps/fiber/main.cpp` only delegates to `fiber::app::run`.

### Client — `src/client/`

Owns the attached process side: raw-terminal setup/restoration, `SIGWINCH` observation, prefix
parsing, input/resize packet emission, daemon output forwarding, and detach behavior. It holds no
pane or terminal-emulator state.

### Daemon — `src/daemon/`

Owns the single per-user socket path, locking, stale-socket validation, daemonization, listener
lifetime, and cleanup. It creates and owns the listener and lends the descriptor to the core engine.
Workspace creation, lookup, listing, and removal are handled by the authoritative engine.

### Core — `src/core/`

Owns up to 64 running workspaces in one reactor. Each currently contains one child process, PTY,
`vt::Terminal`, attached daemon-side client descriptor, protocol-message state, frame scheduling,
and backpressure state. The reactor borrows the daemon listener and remains the sole owner of
mutable workspace and terminal state.

### Supporting components

- `src/platform/`: descriptor I/O, PTY/process operations, and terminal mode;
- `src/protocol/`: bounded packet encoding, prefix parsing, and incremental decoding;
- `src/render/`: retained frame buffers and partial nonblocking client writes;
- `src/terminal/`: the sole private `libghostty-vt` adapter.

## Supported behavior

The runtime currently provides:

- up to 64 validated named workspaces in one per-user daemon;
- one shell, PTY, and canonical Ghostty terminal per workspace;
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

- workspaces have no task store, views, or split layout tree;
- only one client may attach at a time;
- the local protocol has no version or capability negotiation;
- listener acceptance and initial attach setup still use the vertical slice's simple policy;
- new workspaces inherit the daemon's original environment and working directory;
- extension commands/events and configurable key tables are not integrated;
- the renderer does not yet compose multiple pane surfaces.

These are feature and runtime-hardening tasks, not reasons to reorganize the source tree again.

## Build-out sequence

1. Introduce dense generational stores for workspaces, tasks, runs, views, and clients.
2. Represent topology and lifecycle changes as typed core commands.
3. Generalize listener integration to multiple pending clients without blocking workspace progress.
4. Replace the rebuilt poll set with the descriptor registry/reactor abstraction.
5. Add split-tree views and per-client viewport state.
6. Generalize rendering from one terminal surface to composed surface rectangles.
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
