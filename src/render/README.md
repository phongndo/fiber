# Renderer

Transforms terminal damage, mux layout, and client view state into bounded outer-terminal frames.
It owns layout composition, clipping, borders/status/overlays, retained physical client state,
diffing, and ANSI output encoding.

Rendering must be synchronous, deterministic, allocation-bounded, and limited to affected clients
and rows. It does not poll descriptors, write sockets, mutate mux topology, parse application VT
streams, or execute extensions.

The pane compositor accepts bounded, already-resolved surface rectangles, validates them before
consuming terminal damage, offsets pane output into one synchronized frame, and gives cursor and
outer-terminal mode ownership to the focused pane. The engine feeds it resolved split-tree surfaces
with one-cell separators. Status rows, pane-number overlays, and generalized UI surfaces remain
future renderer inputs.
