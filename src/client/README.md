# Client

Home of the disposable attached-terminal client.

**Owns:** raw-terminal lifetime, local input/prefix handling, resize forwarding, daemon connection
lifetime, outer-terminal byte writes, and reliable terminal restoration.

**Does not own:** sessions, panes, PTYs, terminal emulation, layouts, or daemon rendering state. It
uses platform mechanisms and protocol values and can be terminated without affecting the session.
The implementation connects through the daemon's session endpoint interface but never accesses core
state directly.
