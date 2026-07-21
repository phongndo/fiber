#include "protocol/single_pane.hpp"

#include "fiber/assert.hpp"

#include <cstring>

namespace fiber::protocol {
namespace {

constexpr std::byte packet_input{'I'};
constexpr std::byte packet_resize{'R'};
constexpr std::byte packet_detach{'D'};

void encode_u16(const std::uint16_t value, const std::span<std::byte, 2> output) noexcept {
  output.front() = static_cast<std::byte>(value >> 8U);
  output.back() = static_cast<std::byte>(value & 0xFFU);
}

[[nodiscard]] constexpr auto decode_u16(const std::byte high, const std::byte low) noexcept
    -> std::uint16_t {
  const auto high_value = std::to_integer<std::uint16_t>(high);
  const auto low_value = std::to_integer<std::uint16_t>(low);
  return static_cast<std::uint16_t>((high_value << 8U) | low_value);
}

[[nodiscard]] auto encode_dimensions_packet(const std::byte type,
                                            const Dimensions dimensions) noexcept
    -> std::array<std::byte, 5> {
  std::array<std::byte, 5> packet{type};
  encode_u16(dimensions.columns, std::span(packet).subspan<1, 2>());
  encode_u16(dimensions.rows, std::span(packet).subspan<3, 2>());
  return packet;
}

} // namespace

[[nodiscard]] auto encode_workspace_header(const ControlCommand command,
                                           const std::string_view workspace) noexcept
    -> std::array<std::byte, 2> {
  FIBER_ASSERT(!workspace.empty());
  FIBER_ASSERT(workspace.size() <= workspace_name_bytes_max);
  return {wire_byte(command), static_cast<std::byte>(workspace.size())};
}

[[nodiscard]] auto encode_dimensions(const Dimensions dimensions) noexcept
    -> std::array<std::byte, 4> {
  std::array<std::byte, 4> packet{};
  encode_u16(dimensions.columns, std::span(packet).subspan<0, 2>());
  encode_u16(dimensions.rows, std::span(packet).subspan<2, 2>());
  return packet;
}

[[nodiscard]] auto encode_resize(const Dimensions dimensions) noexcept -> std::array<std::byte, 5> {
  return encode_dimensions_packet(packet_resize, dimensions);
}

[[nodiscard]] auto encode_input_header(const std::size_t bytes) noexcept
    -> std::array<std::byte, 3> {
  FIBER_ASSERT(bytes <= input_bytes_max * 2U);
  std::array<std::byte, 3> header{packet_input};
  encode_u16(static_cast<std::uint16_t>(bytes), std::span(header).subspan<1, 2>());
  return header;
}

[[nodiscard]] auto encode_detach() noexcept -> std::array<std::byte, 1> { return {packet_detach}; }

[[nodiscard]] auto decode_dimensions(const std::span<const std::byte, 4> bytes) noexcept
    -> Dimensions {
  return {
      .columns = decode_u16(bytes.front(), bytes.subspan<1>().front()),
      .rows = decode_u16(bytes.subspan<2>().front(), bytes.subspan<3>().front()),
  };
}

[[nodiscard]] auto PrefixParser::parse(const std::span<const std::byte> input,
                                       const std::span<std::byte> output) noexcept -> PrefixResult {
  PrefixResult result{};
  for (const auto byte : input) {
    if (prefix_) {
      prefix_ = false;
      if (byte == std::byte{'d'}) {
        result.detach = true;
        break;
      }
      if (byte == std::byte{0x02}) {
        output.subspan(result.bytes, 1).front() = byte;
        ++result.bytes;
        continue;
      }
      output.subspan(result.bytes, 1).front() = std::byte{0x02};
      ++result.bytes;
    } else if (byte == std::byte{0x02}) {
      prefix_ = true;
      continue;
    }

    output.subspan(result.bytes, 1).front() = byte;
    ++result.bytes;
  }
  FIBER_ASSERT(result.bytes <= output.size());
  return result;
}

[[nodiscard]] auto ClientDecoder::writable_bytes() noexcept -> std::span<std::byte> {
  FIBER_ASSERT(pending_size_ == 0);
  return std::span(storage_).subspan(used_);
}

[[nodiscard]] auto ClientDecoder::commit(const std::size_t bytes) noexcept
    -> std::expected<void, DecodeError> {
  if (pending_size_ != 0 || bytes > storage_.size() - used_) {
    return std::unexpected(DecodeError::buffer_full);
  }
  used_ += bytes;
  return {};
}

[[nodiscard]] auto ClientDecoder::next() noexcept
    -> std::expected<std::optional<ClientMessage>, DecodeError> {
  FIBER_ASSERT(pending_size_ == 0);
  if (used_ == 0) {
    return std::optional<ClientMessage>{};
  }

  const auto buffered = std::span(storage_).first(used_);
  const auto type = buffered.front();
  if (type == packet_detach) {
    pending_size_ = 1;
    return ClientMessage{
        .kind = ClientMessageKind::detach,
        .input = {},
    };
  }
  if (type == packet_resize) {
    if (used_ < 5) {
      return std::optional<ClientMessage>{};
    }
    pending_size_ = 5;
    return ClientMessage{
        .kind = ClientMessageKind::resize,
        .dimensions = decode_dimensions(std::span(buffered).subspan<1, 4>()),
        .input = {},
    };
  }
  if (type != packet_input) {
    return std::unexpected(DecodeError::invalid_type);
  }
  if (used_ < 3) {
    return std::optional<ClientMessage>{};
  }

  const auto length = decode_u16(buffered.subspan<1>().front(), buffered.subspan<2>().front());
  if (length > input_bytes_max * 2U) {
    return std::unexpected(DecodeError::input_too_large);
  }
  const auto packet_size = std::size_t{3} + length;
  if (used_ < packet_size) {
    return std::optional<ClientMessage>{};
  }

  pending_size_ = packet_size;
  return ClientMessage{
      .kind = ClientMessageKind::input,
      .input = buffered.subspan(3, length),
  };
}

void ClientDecoder::consume() noexcept {
  FIBER_ASSERT(pending_size_ > 0);
  FIBER_ASSERT(pending_size_ <= used_);
  std::memmove(storage_.data(), std::span(storage_).subspan(pending_size_).data(),
               used_ - pending_size_);
  used_ -= pending_size_;
  pending_size_ = 0;
}

void ClientDecoder::reset() noexcept {
  used_ = 0;
  pending_size_ = 0;
}

} // namespace fiber::protocol
