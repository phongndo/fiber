#include "fiber/config.hpp"

#include <gtest/gtest.h>

#include <string>
#include <string_view>

namespace fiber::config {
namespace {

TEST(ConfigTest, LoadsTypedSettingsFromRestrictedLua) {
  constexpr std::string_view source = R"(
    local values = { 1, 2, 3 }
    return {
      prefix = "C-a",
      frame_delay_us = values[2] * 500,
      scrollback_rows = 20000,
    }
  )";
  const auto result = load(source);
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result->prefix, 0x01);
  EXPECT_EQ(result->frame_delay_us, 1'000U);
  EXPECT_EQ(result->scrollback_rows, 20'000U);
}

TEST(ConfigTest, RejectsAmbientLibrariesAndInvalidSettings) {
  const auto ambient = load("return { value = os.execute('true') }");
  ASSERT_FALSE(ambient.has_value());
  EXPECT_EQ(ambient.error(), Error::invalid_result);

  const auto invalid = load("return { prefix = 'C-?', scrollback_rows = -1 }");
  ASSERT_FALSE(invalid.has_value());
  EXPECT_EQ(invalid.error(), Error::invalid_result);
}

TEST(ConfigTest, EnforcesSourceMemoryAndInstructionLimits) {
  const std::string oversized(source_bytes_max + 1U, ' ');
  const auto source_result = load(oversized);
  ASSERT_FALSE(source_result.has_value());
  EXPECT_EQ(source_result.error(), Error::source_too_large);

  const auto loop =
      load("while true do end", {.memory_bytes = memory_bytes_default, .instructions = 2'000});
  ASSERT_FALSE(loop.has_value());
  EXPECT_EQ(loop.error(), Error::instruction_limit);

  const auto memory =
      load("return { value = string.rep('x', 100000) }",
           {.memory_bytes = std::size_t{32} * 1'024U, .instructions = instructions_default});
  ASSERT_FALSE(memory.has_value());
  EXPECT_EQ(memory.error(), Error::out_of_memory);
}

TEST(ConfigTest, UsesDefaultsForOmittedFields) {
  const auto result = load("return {}");
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(*result, Settings{});
}

} // namespace
} // namespace fiber::config
