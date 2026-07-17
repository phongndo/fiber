#ifndef FIBER_CORE_ENGINE_HPP
#define FIBER_CORE_ENGINE_HPP

#include <string_view>

namespace fiber::core {

using EndpointRelease = void (*)(void* context) noexcept;

// Runs the authoritative bounded reactor for one session. The engine invokes release_endpoint
// exactly once after it stops using the borrowed listener and before it waits for the child; the
// daemon retains ownership of the listener and its filesystem lifecycle.
[[nodiscard]] auto run_session(int listener, std::string_view session,
                               EndpointRelease release_endpoint, void* release_context) noexcept
    -> int;

} // namespace fiber::core

#endif // FIBER_CORE_ENGINE_HPP
