#ifndef WRVISA_RESOURCE_RESOURCE_PARSER_H
#define WRVISA_RESOURCE_RESOURCE_PARSER_H

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "visa.h"

namespace wrvisa {

enum class ResourceKind {
    asrl_instr,
    gpib_instr,
    gpib_intfc,
    tcpip_instr,
    tcpip_socket,
    usb_instr,
    project_mock,
};

struct ResourceDescriptor {
    ResourceKind kind{};
    ViUInt16 interface_type{0};
    ViUInt16 interface_number{0};
    std::string resource_class;
    std::string canonical_name;
    std::string host;
    ViUInt16 port{0};
    std::string device_name;
};

std::optional<ResourceDescriptor> parse_resource(std::string_view value);

}  // namespace wrvisa

#endif
