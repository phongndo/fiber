#include "protocol/single_pane.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <span>
#include <string_view>

#include <gtest/gtest.h>

namespace fiber::protocol {
namespace {

// GoogleTest assertion macros inflate the measured branch count.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST(ProtocolTest, DecodesFragmentedInputPacket) {
  ClientDecoder decoder;
  const std::array payload{std::byte{'a'}, std::byte{'b'}, std::byte{'c'}};
  const auto header = encode_input_header(payload.size());

  auto writable = decoder.writable_bytes();
  std::ranges::copy(header, writable.begin());
  ASSERT_TRUE(decoder.commit(header.size()).has_value());
  const auto incomplete = decoder.next();
  ASSERT_TRUE(incomplete.has_value());
  EXPECT_FALSE(incomplete->has_value());

  writable = decoder.writable_bytes();
  std::ranges::copy(payload, writable.begin());
  ASSERT_TRUE(decoder.commit(payload.size()).has_value());
  const auto decoded = decoder.next();
  // value() turns malformed or incomplete test output into an explicit test exception.
  // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
  const auto& message = decoded.value().value();
  EXPECT_EQ(message.kind, ClientMessageKind::input);
  EXPECT_TRUE(std::ranges::equal(message.input, payload));

  decoder.consume();
  const auto empty = decoder.next();
  ASSERT_TRUE(empty.has_value());
  EXPECT_FALSE(empty->has_value());
}

// GoogleTest assertion macros inflate the measured branch count.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST(ProtocolTest, DecodesResizeAndDetachPackets) {
  ClientDecoder decoder;
  const auto resize = encode_resize({.columns = 132, .rows = 43});
  const auto detach = encode_detach();
  auto writable = decoder.writable_bytes();
  auto destination = std::ranges::copy(resize, writable.begin()).out;
  std::ranges::copy(detach, destination);
  ASSERT_TRUE(decoder.commit(resize.size() + detach.size()).has_value());

  const auto decoded_resize = decoder.next();
  // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
  const auto& resize_message = decoded_resize.value().value();
  EXPECT_EQ(resize_message.kind, ClientMessageKind::resize);
  EXPECT_EQ(resize_message.dimensions.columns, 132);
  EXPECT_EQ(resize_message.dimensions.rows, 43);
  decoder.consume();

  const auto decoded_detach = decoder.next();
  // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
  EXPECT_EQ(decoded_detach.value().value().kind, ClientMessageKind::detach);
}

TEST(ProtocolTest, EncodesNamedAttachControlFields) {
  constexpr std::string_view workspace = "project";

  const auto header = encode_workspace_header(ControlCommand::attach, workspace);
  const auto dimensions = encode_dimensions({.columns = 132, .rows = 43});

  EXPECT_EQ(header.front(), wire_byte(ControlCommand::attach));
  EXPECT_EQ(decode_workspace_name_size(header.back()), workspace.size());
  const auto decoded_dimensions = decode_dimensions(dimensions);
  EXPECT_EQ(decoded_dimensions.columns, 132);
  EXPECT_EQ(decoded_dimensions.rows, 43);
}

TEST(ProtocolTest, RejectsUnknownPacketType) {
  ClientDecoder decoder;
  decoder.writable_bytes().front() = std::byte{'?'};
  ASSERT_TRUE(decoder.commit(1).has_value());
  const auto decoded = decoder.next();
  ASSERT_FALSE(decoded.has_value());
  EXPECT_EQ(decoded.error(), DecodeError::invalid_type);
}

TEST(ProtocolTest, RejectsOversizedInputBeforeReceivingPayload) {
  ClientDecoder decoder;
  const std::array header{std::byte{'I'}, std::byte{0x20}, std::byte{0x01}};
  std::ranges::copy(header, decoder.writable_bytes().begin());
  ASSERT_TRUE(decoder.commit(header.size()).has_value());

  const auto decoded = decoder.next();

  ASSERT_FALSE(decoded.has_value());
  EXPECT_EQ(decoded.error(), DecodeError::input_too_large);
}

TEST(ProtocolTest, PrefixParserDetachesWithoutForwardingCommand) {
  PrefixParser parser;
  const std::array input{std::byte{'x'}, std::byte{0x02}, std::byte{'d'}, std::byte{'y'}};
  std::array<std::byte, input.size() * 2U> output{};

  const auto result = parser.parse(input, output);

  EXPECT_TRUE(result.detach);
  ASSERT_EQ(result.bytes, 1);
  EXPECT_EQ(output.front(), std::byte{'x'});
}

TEST(ProtocolTest, PrefixParserForwardsLiteralPrefix) {
  PrefixParser parser;
  const std::array input{std::byte{0x02}, std::byte{0x02}};
  std::array<std::byte, input.size() * 2U> output{};

  const auto result = parser.parse(input, output);

  EXPECT_FALSE(result.detach);
  ASSERT_EQ(result.bytes, 1);
  EXPECT_EQ(output.front(), std::byte{0x02});
}

} // namespace
} // namespace fiber::protocol
