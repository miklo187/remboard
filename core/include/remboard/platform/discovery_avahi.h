#pragma once

#include <memory>

#include "remboard/idiscovery.h"

namespace remboard {

// Linux mDNS/Avahi-backed IDiscovery implementation. Declared here (rather
// than only in core/src) so app-linux can construct one without reaching
// into core's private headers; avahi types stay confined to the .cpp.
std::unique_ptr<IDiscovery> make_avahi_discovery();

}  // namespace remboard
