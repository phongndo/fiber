# Platform

Narrow wrappers for operating-system mechanisms: owned descriptors, PTYs, child processes, Unix
sockets, polling, signals, clocks, and raw terminal mode.

Platform code performs mechanisms and reports explicit results. It does not decide mux policy,
mutate core topology, parse protocol messages, or render frames. Abstract only operations Fiber
actually uses; do not build a speculative portability framework.
