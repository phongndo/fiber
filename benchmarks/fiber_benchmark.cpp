#include "fiber/fiber.hpp"

#include <benchmark/benchmark.h>

namespace fiber {
namespace {

void benchmark_greeting(benchmark::State& state) {
  for ([[maybe_unused]] const auto iteration : state) {
    benchmark::DoNotOptimize(greeting());
  }
}

BENCHMARK(benchmark_greeting);

} // namespace
} // namespace fiber
