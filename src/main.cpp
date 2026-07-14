#include "fiber/fiber.hpp"

#include <iostream>

int main() {
  std::cout << fiber::greeting() << '\n';
  return 0;
}
