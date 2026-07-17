# Daemon

Home of daemon lifecycle and session transport policy.

**Owns:** session naming, locking and discovery, local listener lifecycle, daemonization, connection
endpoint creation, socket cleanup, and shutdown/control orchestration.

**Does not own:** pane terminal state, PTY scheduling, rendering algorithms, protocol parsing
internals, or raw client-terminal state. It creates a listener and lends it to the core engine for
the bounded session reactor. The engine notifies it to release the endpoint before waiting for the
child to exit.
