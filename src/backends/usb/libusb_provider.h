#ifndef WRVISA_BACKENDS_USB_LIBUSB_PROVIDER_H
#define WRVISA_BACKENDS_USB_LIBUSB_PROVIDER_H

#include <cstdint>
#include <optional>
#include <vector>

#include "backends/usb/usb_transport.h"

namespace wrvisa {

// Registers the process-wide production provider when libusb 1.0.30 was
// available at build time. Safe to call repeatedly and concurrently.
void ensure_libusb_provider_registered();

namespace libusb_detail {

struct EndpointDescriptor {
    std::uint8_t address{0};
    std::uint8_t attributes{0};
    std::uint16_t maximum_packet_size{0};
};

struct InterfaceDescriptor {
    std::uint8_t number{0};
    std::uint8_t alternate_setting{0};
    std::uint8_t interface_class{0};
    std::uint8_t interface_subclass{0};
    std::uint8_t interface_protocol{0};
    std::vector<EndpointDescriptor> endpoints;
};

std::optional<UsbInterfaceInfo> inspect_interface(
    const InterfaceDescriptor& descriptor) noexcept;
std::optional<UsbInterfaceInfo> inspect_raw_interface(
    const InterfaceDescriptor& descriptor,
    const UsbRawConfiguration& configuration) noexcept;
ViStatus map_error(int error, bool opening) noexcept;
bool provider_available() noexcept;

}  // namespace libusb_detail
}  // namespace wrvisa

#endif
