#ifndef FIBER_FIBER_HPP
#define FIBER_FIBER_HPP

#include <cstdint>
#include <span>
#include <string_view>

namespace fiber {

[[nodiscard]] auto greeting() noexcept -> std::string_view;
[[nodiscard]] auto ghostty_version() noexcept -> std::span<const std::uint8_t>;

} // namespace fiber

#endif // FIBER_FIBER_HPP
