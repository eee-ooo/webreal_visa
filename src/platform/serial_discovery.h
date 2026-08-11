#ifndef WRVISA_PLATFORM_SERIAL_DISCOVERY_H
#define WRVISA_PLATFORM_SERIAL_DISCOVERY_H

#include <map>
#include <string>

#include "visa.h"

namespace wrvisa {

std::map<ViUInt16, std::string> discover_serial_ports() noexcept;

}  // namespace wrvisa

#endif
