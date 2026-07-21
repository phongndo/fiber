#ifndef FIBER_DAEMON_SERVER_HPP
#define FIBER_DAEMON_SERVER_HPP

#include <string_view>

namespace fiber::daemon {

inline constexpr std::string_view default_workspace = "default";

[[nodiscard]] auto validate_workspace(std::string_view workspace) noexcept -> bool;

// Connects to the per-user daemon. Ownership of the returned descriptor transfers to the caller;
// -1 means the daemon is unavailable.
[[nodiscard]] auto open_server_connection() -> int;

[[nodiscard]] auto ensure(std::string_view workspace) -> int;
[[nodiscard]] auto start(std::string_view workspace = default_workspace) -> int;
[[nodiscard]] auto list() -> int;
[[nodiscard]] auto list(std::string_view workspace) -> int;
[[nodiscard]] auto kill(std::string_view workspace = default_workspace) -> int;
[[nodiscard]] auto kill_all() -> int;

} // namespace fiber::daemon

#endif // FIBER_DAEMON_SERVER_HPP
