# Terminal adapter

Home of Fiber's `libghostty-vt` adapter.

This is the only component allowed to include `libghostty-vt` headers. It feeds PTY bytes and input
events to Ghostty and exposes Fiber-owned terminal sizes, effects, damage, cells, and bounded output
values. Ghostty types must never appear in another component's interface.

`libghostty-vt` is a private implementation dependency. This component does not own PTYs, child
processes, layouts, multi-pane composition, client sockets, or scheduling.
