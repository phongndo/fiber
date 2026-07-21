#ifndef FIBER_CORE_ENGINE_HPP
#define FIBER_CORE_ENGINE_HPP

namespace fiber::core {

using EndpointRelease = void (*)(void* context) noexcept;

// Runs the authoritative bounded reactor for every workspace. The engine invokes release_endpoint
// exactly once after it stops using the borrowed listener; the daemon retains ownership of the
// listener and its filesystem lifecycle.
[[nodiscard]] auto run_server(int listener, EndpointRelease release_endpoint,
                              void* release_context) noexcept -> int;

} // namespace fiber::core

#endif // FIBER_CORE_ENGINE_HPP
