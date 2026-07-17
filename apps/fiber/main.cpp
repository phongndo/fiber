#include "app/application.hpp"

int main(const int argument_count, char** argument_values) {
  return fiber::app::run(argument_count, argument_values);
}
