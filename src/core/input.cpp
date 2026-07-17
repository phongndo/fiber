#include "core/input.hpp"

#include "fiber/assert.hpp"
#include "platform/io.hpp"
#include "protocol/single_pane.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace fiber::core {
namespace {

[[nodiscard]] constexpr auto control_key(const std::byte byte) noexcept -> vt::Key {
  const auto value = std::to_integer<std::uint8_t>(byte);
  FIBER_ASSERT(value >= 1 && value <= 26);
  return static_cast<vt::Key>(static_cast<std::uint8_t>(vt::Key::a) + value - 1U);
}

[[nodiscard]] constexpr auto arrow_key(const std::byte final) noexcept -> vt::Key {
  switch (std::to_integer<char>(final)) {
  case 'A':
    return vt::Key::arrow_up;
  case 'B':
    return vt::Key::arrow_down;
  case 'C':
    return vt::Key::arrow_right;
  case 'D':
    return vt::Key::arrow_left;
  case 'F':
    return vt::Key::end;
  case 'H':
    return vt::Key::home;
  default:
    return vt::Key::unidentified;
  }
}

} // namespace

// This is the bounded translation state machine for legacy terminal input.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
[[nodiscard]] auto write_normalized_input(const int pty, vt::Terminal& terminal,
                                          const std::span<const std::byte> input) noexcept -> bool {
  std::array<std::byte, protocol::input_bytes_max * 4U> encoded{};
  std::size_t input_offset = 0;
  std::size_t output_size = 0;
  while (input_offset < input.size()) {
    vt::KeyEvent event{};
    std::array<char, 1> event_text{};
    std::size_t consumed = 0;
    const auto byte = input.subspan(input_offset, 1).front();
    const auto value = std::to_integer<std::uint8_t>(byte);
    if (byte == std::byte{0x0D}) {
      // Enter and Ctrl-M share the same legacy byte. Treat it as the functional Enter key so
      // applications using Kitty keyboard modes receive the semantic key instead of Ctrl-M.
      event.key = vt::Key::enter;
      consumed = 1;
    } else if (byte == std::byte{0x09}) {
      event.key = vt::Key::tab;
      consumed = 1;
    } else if (byte == std::byte{0x7F}) {
      event.key = vt::Key::backspace;
      consumed = 1;
    } else if (value >= 1 && value <= 26) {
      event.key = control_key(byte);
      event.modifiers = vt::key_modifier_control;
      event.unshifted_codepoint = static_cast<std::uint32_t>('a' + value - 1U);
      event_text.front() = static_cast<char>('a' + value - 1U);
      event.text = std::string_view(event_text.data(), event_text.size());
      consumed = 1;
    } else if (input.size() - input_offset >= 3 && byte == std::byte{0x1B} &&
               (input.subspan(input_offset + 1, 1).front() == std::byte{'['} ||
                input.subspan(input_offset + 1, 1).front() == std::byte{'O'})) {
      event.key = arrow_key(input.subspan(input_offset + 2, 1).front());
      consumed = event.key == vt::Key::unidentified ? 0 : 3;
    }

    if (consumed == 0) {
      if (output_size == encoded.size() &&
          !platform::write_all(pty, std::span(encoded).first(output_size))) {
        return false;
      }
      if (output_size == encoded.size()) {
        output_size = 0;
      }
      std::span(encoded).subspan(output_size, 1).front() = byte;
      ++output_size;
      ++input_offset;
      continue;
    }

    constexpr std::size_t key_bytes_max = 128;
    if (encoded.size() - output_size < key_bytes_max) {
      if (!platform::write_all(pty, std::span(encoded).first(output_size))) {
        return false;
      }
      output_size = 0;
    }
    auto available = std::span(encoded).subspan(output_size);
    const auto key_bytes = terminal.encode_key(event, available);
    if (!key_bytes.has_value()) {
      return false;
    }
    output_size += *key_bytes;
    input_offset += consumed;
  }
  return output_size == 0 || platform::write_all(pty, std::span(encoded).first(output_size));
}

} // namespace fiber::core
