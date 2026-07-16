#ifndef FIBER_CONFIG_HPP
#define FIBER_CONFIG_HPP

#include "fiber/limits.hpp"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <string_view>

namespace fiber::config {

inline constexpr std::size_t source_bytes_max = std::size_t{64} * 1'024U;
inline constexpr std::size_t memory_bytes_default = std::size_t{4} * 1'024U * 1'024U;
inline constexpr std::uint64_t instructions_default = 1'000'000;

struct Settings final {
  std::uint8_t prefix{0x02};
  std::uint32_t frame_delay_us{2'000};
  std::size_t scrollback_rows{limits::terminal_scrollback_rows_default};

  friend constexpr auto operator==(const Settings&, const Settings&) noexcept -> bool = default;
};

enum class Error : std::uint8_t {
  source_too_large,
  out_of_memory,
  syntax,
  instruction_limit,
  invalid_result,
};

struct Limits final {
  std::size_t memory_bytes{memory_bytes_default};
  std::uint64_t instructions{instructions_default};
};

// Executes a restricted Lua chunk that must return a configuration table.
[[nodiscard]] auto load(std::string_view source, Limits limits = {}) noexcept
    -> std::expected<Settings, Error>;

} // namespace fiber::config

#endif // FIBER_CONFIG_HPP
