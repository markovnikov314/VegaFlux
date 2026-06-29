#include <cassert>
#include <string_view>

#include "aegis_events/version.hpp"

int main() {
  using namespace vegaflux::aegis_events;
  assert(kProjectName == std::string_view{"VegaFlux"});
  assert(kSchemaVersion == std::string_view{"vegaflux.canonical_market.v0.1"});
  return 0;
}
