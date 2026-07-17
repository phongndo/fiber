# Renderer

Transforms terminal damage, mux layout, and client view state into bounded outer-terminal frames.
It owns layout composition, clipping, borders/status/overlays, retained physical client state,
diffing, and ANSI output encoding.

Rendering must be synchronous, deterministic, allocation-bounded, and limited to affected clients
and rows. It does not poll descriptors, write sockets, mutate mux topology, parse application VT
streams, or execute extensions.
