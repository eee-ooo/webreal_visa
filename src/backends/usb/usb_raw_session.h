#ifndef WRVISA_BACKENDS_USB_USB_RAW_SESSION_H
#define WRVISA_BACKENDS_USB_USB_RAW_SESSION_H

#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>

#include "backends/usb/usb_transport.h"
#include "core/backend_session.h"

namespace wrvisa {

class UsbRawBackendSession final : public BackendSession {
public:
    UsbRawBackendSession(std::unique_ptr<UsbTransport> transport,
                         UsbRawConfiguration configuration)
        : transport_(std::move(transport)),
          configuration_(configuration),
          interface_(transport_->interface_info()) {}

    ViStatus read(Operation& operation, ViPBuf buffer, ViUInt32 count,
                  ViPUInt32 return_count, ReadOptions options) override;
    ViStatus write(Operation& operation, ViConstBuf buffer, ViUInt32 count,
                   ViPUInt32 return_count, WriteOptions options) override;
    ViStatus clear() override;
    ViStatus clear(Operation& operation) override;
    ViStatus flush(ViUInt16 mask) override;
    ViStatus read_stb(ViPUInt16 status) override;
    ViStatus assert_trigger(ViUInt16 protocol) override;
    ViStatus usb_control(Operation& operation, std::uint8_t request_type,
                         std::uint8_t request, std::uint16_t value,
                         std::uint16_t index, ViBuf data, ViUInt32 count,
                         ViPUInt32 return_count) override;
    void notify_cancel() noexcept override;
    void close() noexcept override;

private:
    bool valid_configuration() const noexcept;

    std::unique_ptr<UsbTransport> transport_;
    UsbRawConfiguration configuration_;
    UsbInterfaceInfo interface_;
    std::mutex read_mutex_;
    std::mutex write_mutex_;
    std::mutex control_mutex_;
    std::deque<std::uint8_t> read_ahead_;
};

}  // namespace wrvisa

#endif
