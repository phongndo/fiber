# Daemon

Home of per-user daemon lifecycle and transport policy.

**Owns:** the per-user endpoint and lock, local listener lifecycle, daemonization, socket cleanup,
and shutdown orchestration.

**Does not own:** workspace state, terminal state, PTY scheduling, rendering algorithms, protocol
parsing internals, or raw client-terminal state. It creates one listener and lends it to the core
engine, which owns every workspace in the bounded reactor.
