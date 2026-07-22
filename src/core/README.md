# Core

Home of Fiber's authoritative mux engine and dense state.

The current engine owns up to 64 named workspaces and 4,096 panes in one reactor, bounded to 64
panes per workspace. Every pane has one child process, PTY, terminal adapter, and resolved surface.
Each workspace owns its binary split tree, focus/zoom state, attached client, input decoder, damage
schedule, and frame deadline. The engine borrows the daemon's listener but does not own socket
paths, locks, daemonization, or terminal-client raw mode.

As tasks, runs, and views are introduced, this component will move those values into generational
stores and add typed commands/events, bounded work queues, and a generalized backpressure policy. Keep those entities dense and data-oriented rather than creating independently allocated
service objects.

The core may orchestrate platform, terminal, protocol, and render interfaces. It must not know about
CLI syntax, Lua stack details, Unix socket naming policy, or Ghostty C types.
