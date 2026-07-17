#ifndef FIBER_DAEMON_SESSION_HPP
#define FIBER_DAEMON_SESSION_HPP

#include <string_view>

namespace fiber::daemon {

inline constexpr std::string_view default_session = "default";

// Validates and reports invalid user-provided session names.
[[nodiscard]] auto validate_session(std::string_view session) noexcept -> bool;

// Connects to an existing validated session. Ownership of the returned descriptor transfers to the
// caller; -1 means unavailable.
[[nodiscard]] auto open_session_connection(std::string_view session) -> int;

// Starts a session if needed without printing a listing. Used by `new` before client attachment.
[[nodiscard]] auto ensure(std::string_view session) -> int;
[[nodiscard]] auto start(std::string_view session = default_session) -> int;
[[nodiscard]] auto list() -> int;
[[nodiscard]] auto list(std::string_view session) -> int;
[[nodiscard]] auto kill(std::string_view session = default_session) -> int;
[[nodiscard]] auto kill_all() -> int;

} // namespace fiber::daemon

#endif // FIBER_DAEMON_SESSION_HPP
