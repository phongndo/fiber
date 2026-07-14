#include "fiber/fiber.hpp"

#include <ghostty/vt.h>

namespace fiber {

[[nodiscard]] auto greeting() noexcept -> std::string_view { return "Hello, world!"; }

[[nodiscard]] auto ghostty_version() noexcept -> std::span<const std::uint8_t> {
  GhosttyString version{};
  if (ghostty_build_info(GHOSTTY_BUILD_INFO_VERSION_STRING, &version) != GHOSTTY_SUCCESS) {
    return {};
  }

  return {version.ptr, version.len};
}

} // namespace fiber
