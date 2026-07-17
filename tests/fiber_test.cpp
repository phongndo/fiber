#include "fiber/fiber.hpp"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

namespace fiber {
namespace {

TEST(FiberTest, HasGreeting) { EXPECT_THAT(greeting(), testing::StrEq("Hello, world!")); }

TEST(FiberTest, LinksThirdPartyGhosttyVt) { EXPECT_FALSE(ghostty_version().empty()); }

TEST(FiberTest, LinksLuaConfigRuntime) { EXPECT_THAT(lua_version(), testing::HasSubstr("5.5")); }

TEST(FiberTest, LinksZstd) { EXPECT_THAT(zstd_version(), testing::StrEq("1.5.7")); }

} // namespace
} // namespace fiber
