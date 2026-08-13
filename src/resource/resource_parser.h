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
    usb_raw,
    project_mock,
};

enum class TcpipProtocol {
    none,
    vxi11,
    hislip,
    unsupported,
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
    TcpipProtocol tcpip_protocol{TcpipProtocol::none};
    ViUInt16 usb_vendor_id{0};
    ViUInt16 usb_product_id{0};
    std::string usb_serial_number;
    ViUInt16 usb_interface_number{0};
};

std::optional<ResourceDescriptor> parse_resource(std::string_view value);

}  // namespace wrvisa

#endif
