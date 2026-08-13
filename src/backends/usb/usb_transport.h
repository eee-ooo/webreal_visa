#ifndef WRVISA_BACKENDS_USB_USB_TRANSPORT_H
#define WRVISA_BACKENDS_USB_USB_TRANSPORT_H

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include "runtime/operation.h"

namespace wrvisa {

struct UsbControlRequest {
    std::uint8_t request_type{0};
    std::uint8_t request{0};
    std::uint16_t value{0};
    std::uint16_t index{0};
};

struct UsbInterfaceInfo {
    std::uint16_t interface_number{0};
    std::uint8_t bulk_out_endpoint{0};
    std::uint8_t bulk_in_endpoint{0};
    std::uint8_t interrupt_in_endpoint{0};
    std::uint16_t bulk_in_max_packet_size{0};
    bool usb488{false};
    std::uint8_t alternate_setting{0};
    std::uint8_t interrupt_out_endpoint{0};
};

enum class UsbTransferType : std::uint8_t {
    none = 0,
    bulk = 1,
    interrupt = 2,
};

struct UsbRawConfiguration {
    std::uint8_t alternate_setting{0};
    UsbTransferType read_transfer_type{UsbTransferType::none};
    std::uint8_t read_endpoint{0};
    UsbTransferType write_transfer_type{UsbTransferType::none};
    std::uint8_t write_endpoint{0};
};

// Transport-only contract. Implementations own endpoint selection and interface
// claim lifetime; protocol sessions never depend on libusb or platform handles.
class UsbTransport {
public:
    virtual ~UsbTransport() = default;

    virtual UsbInterfaceInfo interface_info() const noexcept { return {}; }
    virtual ViStatus bulk_out(Operation& operation,
                              std::span<const std::uint8_t> data,
                              std::size_t& transferred) = 0;
    virtual ViStatus bulk_in(Operation& operation, std::size_t maximum_size,
                             std::vector<std::uint8_t>& data) = 0;
    virtual ViStatus control_in(Operation& operation,
                                const UsbControlRequest& request,
                                std::size_t response_size,
                                std::vector<std::uint8_t>& response) {
        static_cast<void>(operation);
        static_cast<void>(request);
        static_cast<void>(response_size);
        static_cast<void>(response);
        return VI_ERROR_NSUP_OPER;
    }
    virtual ViStatus control_out(Operation& operation,
                                 const UsbControlRequest& request,
                                 std::span<const std::uint8_t> data,
                                 std::size_t& transferred) {
        static_cast<void>(operation);
        static_cast<void>(request);
        static_cast<void>(data);
        transferred = 0;
        return VI_ERROR_NSUP_OPER;
    }
    virtual ViStatus interrupt_in(Operation& operation,
                                  std::size_t maximum_size,
                                  std::vector<std::uint8_t>& data) {
        static_cast<void>(operation);
        static_cast<void>(maximum_size);
        static_cast<void>(data);
        return VI_ERROR_NSUP_OPER;
    }
    virtual ViStatus interrupt_out(Operation& operation,
                                   std::span<const std::uint8_t> data,
                                   std::size_t& transferred) {
        static_cast<void>(operation);
        static_cast<void>(data);
        transferred = 0;
        return VI_ERROR_NSUP_OPER;
    }
    virtual ViStatus clear_halt(Operation& operation,
                                std::uint8_t endpoint_address) {
        static_cast<void>(operation);
        static_cast<void>(endpoint_address);
        return VI_ERROR_NSUP_OPER;
    }
    virtual void cancel() noexcept = 0;
    virtual void close() noexcept = 0;
};

}  // namespace wrvisa

#endif
