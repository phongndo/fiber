#include "fiber/fiber.hpp"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

namespace fiber {
namespace {

TEST(FiberTest, HasGreeting) { EXPECT_THAT(greeting(), testing::StrEq("Hello, world!")); }

TEST(FiberTest, LinksVendoredGhosttyVt) { EXPECT_FALSE(ghostty_version().empty()); }

} // namespace
} // namespace fiber
