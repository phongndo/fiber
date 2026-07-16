# Fiber architecture

Fiber is a bounded, data-oriented terminal multiplexer. The daemon owns PTYs, terminal state,
sessions, layouts, commands, and per-client render state. A disposable client owns raw terminal
mode and forwards input, resize events, and daemon-produced terminal bytes.

## Invariants

1. Every mutable object has one owner.
2. Every queue has byte and element bounds.
3. Every event-loop stage has a work bound.
4. A client or extension can never block PTY progress.
5. Generational IDs are validated at trust boundaries.
6. State transitions are explicit and exhaustive.
7. Rendering visits only affected clients and rows.
8. Steady-state hot paths do not use the general heap.
9. Nondeterminism is isolated so event-loop executions can be replayed.
10. Capacity exhaustion is a normal, tested result.

External input errors are rejected without damaging daemon state. Internal invariant violations use
release-enabled assertions and terminate rather than continuing with corrupt state.

## Terminal ownership

Each pane owns one `libghostty-vt` terminal as its canonical emulated state. Fiber uses Ghostty for
VT parsing, screen state, scrollback, reflow, input encoding, terminal modes, and dirty render-state
tracking. Fiber owns PTYs, layout, composition, outer-terminal encoding, and client-specific views.

Ghostty's API is unstable, so only `src/vt/terminal.cpp` includes Ghostty headers. The public adapter
uses Fiber value types and opaque ownership. Ghostty effects synchronously append to bounded Fiber
queues; they never call application or extension code.

The adapter gives every pane a quota-tracked allocator. General allocations are permitted while a
pane and its terminal are created. PTY parsing and render-state updates reuse established storage
where the upstream API permits it. Allocation statistics and failures remain observable.

## Current single-pane vertical slice

The first end-to-end slice supports up to 64 named sessions. Each session is currently isolated in
its own daemon process and Unix socket and owns one PTY and shell. This keeps failure domains and hot
state independent while the pane/layout model is still evolving. Session discovery validates socket
ownership and names; names are bounded to 32 safe ASCII characters.

A session socket supports one attached terminal client plus independent `list` and `kill` control
connections. Input and resize messages use a bounded binary protocol. `C-b d` detaches the client
without terminating the shell; reattachment reconstructs the visible screen from Ghostty's
canonical state before live output continues.

The current renderer consumes Ghostty dirty rows, retains bounded per-cell physical fingerprints,
trims unchanged row prefixes and suffixes, and emits terminal scroll operations for exact vertical
shifts. Trailing default cells use erase-line rather than space runs. PTY reads are drained in
bounded 256 KiB batches and
coalesced behind a 2 ms frame deadline. Steady-state client writes are nonblocking: one frame may be
in flight while later Ghostty damage accumulates for the next frame. Application escape sequences
never reach the outer terminal, preventing duplicate terminal-query responses and nested
alternate-screen corruption. Common control and navigation input is normalized and re-encoded by
Ghostty against each pane's current legacy or Kitty keyboard modes. Full-frame formatting remains
only as a diagnostic fallback.

## Configuration runtime

Lua 5.5 is Fiber's configuration language. `fiber::config::load` executes source in a dedicated,
quota-bound Lua state and compiles the returned table into typed immutable settings. Source size,
instruction count, and allocator bytes are bounded. Only base, table, string, math, and UTF-8
libraries are opened; file loading, process execution, dynamic native modules, debug access, and
explicit garbage-collector control are unavailable.

The current typed settings cover prefix, frame coalescing delay, and scrollback limits. Startup-file
selection, atomic reload, key tables, commands, layouts, status components, and extension
declarations remain integration work. Lua callbacks must never run in PTY parsing, input delivery,
or frame encoding paths.

Zstandard 1.5.7 is available for bounded session snapshots, protocol payloads, and application-owned
history storage. Ghostty's internal scrollback compression remains owned by `libghostty-vt`.

## Data path

Each bounded reactor turn proceeds in stages:

1. collect ready descriptors;
2. read PTYs into pooled slabs;
3. parse bounded byte batches;
4. collect dirty pane IDs;
5. compose dirty rows for affected clients;
6. encode ANSI output batches;
7. flush bounded client queues; and
8. dispatch bounded command and event batches.

The initial implementation uses one nonblocking reactor owner. Pane ownership can later be sharded
without changing the command, ID, or terminal adapter boundaries if end-to-end benchmarks justify
it.
