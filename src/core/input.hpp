#ifndef FIBER_CORE_INPUT_HPP
#define FIBER_CORE_INPUT_HPP

#include "fiber/terminal/terminal.hpp"

#include <cstddef>
#include <span>

namespace fiber::core {

// Normalizes attached-terminal legacy bytes and asks the pane terminal to encode recognized keys
// against its active keyboard modes. Unrecognized bytes are preserved verbatim.
[[nodiscard]] auto write_normalized_input(int pty, vt::Terminal& terminal,
                                          std::span<const std::byte> input) noexcept -> bool;

} // namespace fiber::core

#endif // FIBER_CORE_INPUT_HPP
