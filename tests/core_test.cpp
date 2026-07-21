#include "fiber/bounded_byte_queue.hpp"
#include "fiber/id.hpp"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>

namespace fiber {
namespace {

TEST(GenerationalIdTest, InvalidUntilCreatedFromValidParts) {
  const WorkspaceId invalid;
  const auto workspace = WorkspaceId::from_parts(7, 3);

  EXPECT_FALSE(invalid.is_valid());
  EXPECT_FALSE(WorkspaceId::try_from_parts(7, 0).has_value());
  EXPECT_FALSE(
      WorkspaceId::try_from_parts(std::numeric_limits<std::uint32_t>::max(), 3).has_value());
  EXPECT_TRUE(workspace.is_valid());
  EXPECT_EQ(workspace.slot(), 7U);
  EXPECT_EQ(workspace.generation(), 3U);
}

TEST(BoundedByteQueueTest, PreservesOrderAcrossWraparound) {
  BoundedByteQueue<5> queue;
  const std::array first{std::byte{1}, std::byte{2}, std::byte{3}, std::byte{4}};
  ASSERT_TRUE(queue.append(first));

  std::array<std::byte, 3> first_output{};
  EXPECT_EQ(queue.read(first_output), first_output.size());
  EXPECT_THAT(first_output, testing::ElementsAre(std::byte{1}, std::byte{2}, std::byte{3}));

  const std::array second{std::byte{5}, std::byte{6}, std::byte{7}};
  ASSERT_TRUE(queue.append(second));

  std::array<std::byte, 4> second_output{};
  EXPECT_EQ(queue.read(second_output), second_output.size());
  EXPECT_THAT(second_output,
              testing::ElementsAre(std::byte{4}, std::byte{5}, std::byte{6}, std::byte{7}));
  EXPECT_TRUE(queue.empty());
}

TEST(BoundedByteQueueTest, RejectsInputWithoutPartiallyAppending) {
  BoundedByteQueue<3> queue;
  const std::array first{std::byte{1}, std::byte{2}};
  const std::array too_large{std::byte{3}, std::byte{4}};

  ASSERT_TRUE(queue.append(first));
  EXPECT_FALSE(queue.append(too_large));
  EXPECT_EQ(queue.size(), first.size());

  std::array<std::byte, 3> output{};
  EXPECT_EQ(queue.read(output), first.size());
  EXPECT_THAT(std::span(output).first(first.size()),
              testing::ElementsAre(std::byte{1}, std::byte{2}));
}

} // namespace
} // namespace fiber
