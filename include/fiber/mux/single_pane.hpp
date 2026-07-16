#ifndef FIBER_MUX_SINGLE_PANE_HPP
#define FIBER_MUX_SINGLE_PANE_HPP

#include <string_view>

namespace fiber::mux {

inline constexpr std::string_view default_session = "default";

[[nodiscard]] auto start(std::string_view session = default_session) -> int;
[[nodiscard]] auto attach(std::string_view session = default_session) -> int;
[[nodiscard]] auto create_and_attach(std::string_view session = default_session) -> int;
[[nodiscard]] auto list() -> int;
[[nodiscard]] auto list(std::string_view session) -> int;
[[nodiscard]] auto kill(std::string_view session = default_session) -> int;
[[nodiscard]] auto kill_all() -> int;

} // namespace fiber::mux

#endif // FIBER_MUX_SINGLE_PANE_HPP
