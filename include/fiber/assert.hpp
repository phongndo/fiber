#ifndef FIBER_ASSERT_HPP
#define FIBER_ASSERT_HPP

#include <source_location>

namespace fiber {

[[noreturn]] void
assertion_failed(const char* expression,
                 std::source_location location = std::source_location::current()) noexcept;

} // namespace fiber

#define FIBER_ASSERT(expression)                                                                   \
  (static_cast<bool>(expression)                                                                   \
       ? static_cast<void>(0)                                                                      \
       : ::fiber::assertion_failed(#expression, std::source_location::current()))

#endif // FIBER_ASSERT_HPP
