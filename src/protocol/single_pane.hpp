#ifndef FIBER_PROTOCOL_SINGLE_PANE_HPP
#define FIBER_PROTOCOL_SINGLE_PANE_HPP

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string_view>

namespace fiber::protocol {

inline constexpr std::size_t input_bytes_max = std::size_t{4} * 1'024U;
inline constexpr std::size_t parser_bytes_max = std::size_t{16} * 1'024U;
inline constexpr std::uint16_t columns_max = 500;
inline constexpr std::uint16_t rows_max = 200;
inline constexpr std::size_t workspace_name_bytes_max = 32;

struct Dimensions final {
  std::uint16_t columns{80};
  std::uint16_t rows{24};
};

enum class ControlCommand : std::uint8_t {
  attach = 'A',
  create = 'N',
  list = 'L',
  list_workspace = 'Q',
  kill = 'K',
  kill_all = 'X',
};

enum class ControlResponse : std::uint8_t {
  ready = 'Y',
  busy = 'B',
  missing = 'M',
  capacity = 'C',
  failed = 'F',
};

enum class ClientMessageKind : std::uint8_t {
  input,
  resize,
  detach,
};

enum class DecodeError : std::uint8_t {
  invalid_type,
  input_too_large,
  buffer_full,
};

struct ClientMessage final {
  ClientMessageKind kind{ClientMessageKind::detach};
  Dimensions dimensions{};
  std::span<const std::byte> input;
};

[[nodiscard]] constexpr auto wire_byte(const ControlCommand command) noexcept -> std::byte {
  return static_cast<std::byte>(command);
}

[[nodiscard]] constexpr auto wire_byte(const ControlResponse response) noexcept -> std::byte {
  return static_cast<std::byte>(response);
}

[[nodiscard]] auto encode_workspace_header(ControlCommand command,
                                           std::string_view workspace) noexcept
    -> std::array<std::byte, 2>;
[[nodiscard]] auto encode_dimensions(Dimensions dimensions) noexcept -> std::array<std::byte, 4>;
[[nodiscard]] auto encode_resize(Dimensions dimensions) noexcept -> std::array<std::byte, 5>;
[[nodiscard]] auto encode_input_header(std::size_t bytes) noexcept -> std::array<std::byte, 3>;
[[nodiscard]] auto encode_detach() noexcept -> std::array<std::byte, 1>;
[[nodiscard]] auto decode_dimensions(std::span<const std::byte, 4> bytes) noexcept -> Dimensions;
[[nodiscard]] constexpr auto decode_workspace_name_size(const std::byte value) noexcept
    -> std::size_t {
  return std::to_integer<std::size_t>(value);
}

struct PrefixResult final {
  std::size_t bytes{0};
  bool detach{false};
};

class PrefixParser final {
public:
  [[nodiscard]] auto parse(std::span<const std::byte> input, std::span<std::byte> output) noexcept
      -> PrefixResult;

private:
  bool prefix_{false};
};

// Incremental decoder for the attached-client stream. A returned message borrows decoder storage
// and remains valid until consume() or reset().
class ClientDecoder final {
public:
  [[nodiscard]] auto writable_bytes() noexcept -> std::span<std::byte>;
  [[nodiscard]] auto commit(std::size_t bytes) noexcept -> std::expected<void, DecodeError>;
  [[nodiscard]] auto next() noexcept -> std::expected<std::optional<ClientMessage>, DecodeError>;
  void consume() noexcept;
  void reset() noexcept;

private:
  std::array<std::byte, parser_bytes_max> storage_{};
  std::size_t used_{0};
  std::size_t pending_size_{0};
};

} // namespace fiber::protocol

#endif // FIBER_PROTOCOL_SINGLE_PANE_HPP
