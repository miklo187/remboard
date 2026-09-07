#pragma once

#include <memory>

#include "remboard/idiscovery.h"

namespace remboard {

// Windows IDiscovery implementation, built on the vendored header-only
// mjansson/mdns library (raw Winsock, no external mDNS service required).
// Declared here (rather than only in core/src) so app-windows can construct
// one without reaching into core's private headers, mirroring
// discovery_avahi.h's split for Linux.
std::unique_ptr<IDiscovery> make_mdns_discovery();

}  // namespace remboard
