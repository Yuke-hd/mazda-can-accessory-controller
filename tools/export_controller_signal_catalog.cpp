#include <iostream>

#include "controller_signal_catalog_writer.hpp"
#include "mazda/signal_catalog.hpp"

int main(const int argc, const char *const[]) {
  if (argc != 1)
    return 2;
  controller_signal_catalog::write(std::cout, mazda::internal::signal_catalog());
  return std::cout ? 0 : 1;
}
