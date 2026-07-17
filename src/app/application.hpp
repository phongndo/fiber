#ifndef FIBER_APP_APPLICATION_HPP
#define FIBER_APP_APPLICATION_HPP

namespace fiber::app {

// Owns command-line parsing and dispatch. Process main intentionally delegates here without policy.
[[nodiscard]] auto run(int argument_count, char** argument_values) -> int;

} // namespace fiber::app

#endif // FIBER_APP_APPLICATION_HPP
