# Runtime vertical slice

## Status

Fiber's end-to-end vertical slice has been migrated out of the former monolithic
`single_pane.cpp`. Workspaces now support bounded windows and split panes while retaining the
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

There is no temporary demo component or `app -> demo` dependency. Further mux work must build on
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

Owns up to 64 running workspaces, 1,024 windows, and 4,096 panes in one reactor. Each workspace is
bounded to 16 windows and 64 panes distributed across those windows. Every pane owns one child
process, PTY, terminal, and resolved rectangle. A generationally identified window owns its split
tree and focus/zoom state. The workspace owns its ordered window slots, active-window selection,
attached daemon-side client descriptor, protocol-message state, frame scheduling, and backpressure
state. The reactor borrows the daemon listener and remains the sole owner of mutable workspace and
terminal state.

### Supporting components

- `src/platform/`: descriptor I/O, PTY/process operations, and terminal mode;
- `src/protocol/`: bounded packet encoding, prefix parsing, and incremental decoding;
- `src/render/`: retained frame buffers and partial nonblocking client writes;
- `src/terminal/`: the sole private `libghostty-vt` adapter.

## Supported behavior

The runtime currently provides:

- up to 64 validated named workspaces in one per-user daemon;
- up to 16 windows and 64 shells, PTYs, and canonical Ghostty terminals per workspace;
- generational window IDs that reject stale slot references;
- one attached client plus independent workspace/window-list and kill control connections;
- tmux-compatible window create/cycle/select/kill and pane split/focus/close/zoom bindings;
- one bounded binary split tree per window, with one-cell pane separators;
- a centered, one-row window status with one-based numbers, focused-pane foreground process names,
  and bounded overflow;
- terminal resize propagation from resolved pane rectangles;
- bounded protocol and PTY read batches;
- terminal-generated PTY responses;
- dirty-row rendering and retained physical client state;
- bounded composition of validated pane rectangles into one synchronized outer-terminal frame;
- focused-pane cursor and outer-terminal mode ownership in the composition layer;
- a 2 ms frame-coalescing deadline;
- partial nonblocking live-frame writes;
- full visible-state reconstruction on reattach and active-window changes;
- PTY progress for inactive windows without rendering them;
- deterministic child, descriptor, socket, and lock cleanup.

## Current limitations

The architecture is migrated and the first window/split-pane behavior is implemented, with these
limitations:

- workspaces and panes do not yet use separate generational stores; windows use generational IDs
  within their owning workspace;
- only one client may attach at a time;
- the local protocol has no version or capability negotiation;
- listener acceptance and initial attach setup still use the vertical slice's simple policy;
- new workspaces inherit the daemon's original environment and working directory;
- extension commands/events and configurable key tables are not integrated;
- windows have numeric slots but no user-defined names or interactive rename prompt;
- windows cannot be linked across workspaces;
- pane ratios are fixed at equal halves and cannot yet be resized interactively;
- alternate tmux layouts, pane-number overlays, and per-client physical state are not yet
  implemented.

These are feature and runtime-hardening tasks, not reasons to reorganize the source tree again.

## Build-out sequence

1. Move workspaces, panes, and clients into dense generational stores; window IDs are already
   generational within each workspace.
2. Represent topology and lifecycle changes as typed core commands.
3. Add window naming/rename management and optional status configuration.
4. Generalize listener integration to multiple pending clients without blocking workspace progress.
5. Replace the rebuilt poll set with the descriptor registry/reactor abstraction.
6. Move split topology into generational pane stores and add per-client viewport state.
7. Add adjustable ratios, alternate layouts, pane-number overlays, and status surfaces.
8. Version the protocol and add capabilities, request IDs, and typed errors.
9. Add deferred, budgeted extension commands and immutable events.
10. Add replay traces, capacity tests, fuzzing, and end-to-end latency benchmarks.

## Rules for contributors and agents

- Never move client raw-terminal state into the daemon or core.
- Never move socket naming, locks, or daemonization into the core.
- Never expose Ghostty headers outside `src/terminal/terminal.cpp`.
- Do not let client or extension work block PTY progress.
- Preserve bounds when generalizing a single object into an arena.
- Add state transitions through typed commands instead of direct cross-component mutation.
- Keep structural refactors separate from new mux behavior.
- Update this document when ownership or a runtime invariant changes.
