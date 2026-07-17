#ifndef FIBER_CLIENT_ATTACHED_CLIENT_HPP
#define FIBER_CLIENT_ATTACHED_CLIENT_HPP

#include <string_view>

namespace fiber::client {

[[nodiscard]] auto attach(std::string_view session) -> int;

} // namespace fiber::client

#endif // FIBER_CLIENT_ATTACHED_CLIENT_HPP
