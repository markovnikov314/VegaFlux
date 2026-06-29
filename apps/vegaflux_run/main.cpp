#include <iostream>

#include "aegis_events/version.hpp"

int main() {
  std::cout << vegaflux::aegis_events::kProjectName << " "
            << vegaflux::aegis_events::kSchemaVersion << '\n';
  return 0;
}
