# Core

Home of Fiber's authoritative mux engine and dense state.

The current engine owns up to 64 named workspaces in one reactor. Each workspace currently has one
child process, PTY, terminal adapter, attached client, input decoder, damage schedule, and frame
deadline. The engine borrows the daemon's listener but does not own socket paths, locks,
daemonization, or terminal-client raw mode.

As tasks, runs, views, and multiple surfaces are introduced, this component will also own their
generational stores, layouts, focus, typed commands/events, bounded work queues, and backpressure
policy. Keep those entities dense and data-oriented rather than creating independently allocated
service objects.

The core may orchestrate platform, terminal, protocol, and render interfaces. It must not know about
CLI syntax, Lua stack details, Unix socket naming policy, or Ghostty C types.
