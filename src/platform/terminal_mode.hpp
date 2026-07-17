#ifndef FIBER_PLATFORM_TERMINAL_MODE_HPP
#define FIBER_PLATFORM_TERMINAL_MODE_HPP

#include <cstdint>

#include <termios.h>

namespace fiber::platform {

struct WindowSize final {
  std::uint16_t columns{80};
  std::uint16_t rows{24};
};

[[nodiscard]] auto terminal_size(int descriptor, std::uint16_t columns_max,
                                 std::uint16_t rows_max) noexcept -> WindowSize;

class RawTerminal final {
public:
  RawTerminal() = default;
  RawTerminal(const RawTerminal&) = delete;
  auto operator=(const RawTerminal&) -> RawTerminal& = delete;
  RawTerminal(RawTerminal&&) = delete;
  auto operator=(RawTerminal&&) -> RawTerminal& = delete;
  ~RawTerminal();

  [[nodiscard]] auto enter(int descriptor) noexcept -> bool;

private:
  termios original_{};
  int descriptor_{-1};
  bool active_{false};
};

} // namespace fiber::platform

#endif // FIBER_PLATFORM_TERMINAL_MODE_HPP
