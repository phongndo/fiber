#ifndef FIBER_PLATFORM_PTY_HPP
#define FIBER_PLATFORM_PTY_HPP

#include <cstdint>

#include <sys/types.h>

namespace fiber::platform {

// Spawns the account's login shell with a new controlling PTY. The parent receives the child PID
// and master descriptor; the child replaces itself or exits with status 127.
[[nodiscard]] auto spawn_login_shell(int& pty_descriptor) noexcept -> pid_t;

[[nodiscard]] auto resize_pty(int pty_descriptor, std::uint16_t columns,
                              std::uint16_t rows) noexcept -> bool;

} // namespace fiber::platform

#endif // FIBER_PLATFORM_PTY_HPP
