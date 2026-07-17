#include "core/input.hpp"

#include "fiber/terminal/terminal.hpp"

#include <array>
#include <cstddef>
#include <span>
#include <string_view>
#include <utility>

#include <unistd.h>

#include <gtest/gtest.h>

namespace fiber::core {
namespace {

void write_terminal(vt::Terminal& terminal, const std::string_view bytes) noexcept {
  terminal.write(std::as_bytes(std::span(bytes.data(), bytes.size())));
}

TEST(CoreInputTest, EncodesEnterSemanticallyWhenKittyKeyboardModeIsActive) {
  auto terminal_result = vt::Terminal::create({});
  ASSERT_TRUE(terminal_result.has_value());
  auto terminal = std::move(*terminal_result);
  write_terminal(terminal, "\x1B[>1u");

  std::array<int, 2> descriptors{};
  ASSERT_EQ(::pipe(descriptors.data()), 0);
  const std::array input{std::byte{'l'}, std::byte{'s'}, std::byte{0x0D}};

  ASSERT_TRUE(write_normalized_input(descriptors.back(), terminal, input));
  std::array<std::byte, input.size()> output{};
  const auto bytes_read = ::read(descriptors.front(), output.data(), output.size());

  static_cast<void>(::close(descriptors.front()));
  static_cast<void>(::close(descriptors.back()));
  ASSERT_EQ(bytes_read, static_cast<ssize_t>(output.size()));
  EXPECT_EQ(output, input);
}

TEST(CoreInputTest, SuppliesTextForControlKeys) {
  auto terminal_result = vt::Terminal::create({});
  ASSERT_TRUE(terminal_result.has_value());
  auto terminal = std::move(*terminal_result);

  std::array<int, 2> descriptors{};
  ASSERT_EQ(::pipe(descriptors.data()), 0);
  const std::array input{std::byte{0x03}};

  ASSERT_TRUE(write_normalized_input(descriptors.back(), terminal, input));
  std::array<std::byte, input.size()> output{};
  const auto bytes_read = ::read(descriptors.front(), output.data(), output.size());

  static_cast<void>(::close(descriptors.front()));
  static_cast<void>(::close(descriptors.back()));
  ASSERT_EQ(bytes_read, static_cast<ssize_t>(output.size()));
  EXPECT_EQ(output, input);
}

} // namespace
} // namespace fiber::core
