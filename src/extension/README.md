# Extension host

Hosts configuration and future extensions behind Fiber's typed command/event boundary. Extensions
may register commands, key bindings, subscriptions, and bounded UI components. They receive stable
IDs and immutable snapshots, never pointers into core state.

All execution is deferred and budgeted. Extension code must not run during PTY parsing, input
encoding, composition, or frame encoding; access descriptors; mutate topology directly; or return
unbounded data. Lua is the initial host. A native C++ plugin ABI is not planned.
