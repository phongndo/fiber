# Core

Home of Fiber's authoritative mux engine and dense state.

The current engine owns one pane runtime: child process, PTY, terminal adapter, attached client,
input decoding, damage scheduling, frame deadlines, and bounded reactor work. It borrows the daemon's
listener but does not own socket paths, locks, daemonization, or terminal-client raw mode.

As windows and multiple panes are introduced, this component will also own sessions, windows,
layouts, focus, generational IDs, typed commands/events, bounded work queues, and backpressure
policy. Keep those entities dense and data-oriented rather than creating independently allocated
service objects.

The core may orchestrate platform, terminal, protocol, and render interfaces. It must not know about
CLI syntax, Lua stack details, Unix socket naming policy, or Ghostty C types.
