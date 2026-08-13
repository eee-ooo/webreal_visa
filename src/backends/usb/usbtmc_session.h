#ifndef WRVISA_BACKENDS_USB_USBTMC_SESSION_H
#define WRVISA_BACKENDS_USB_USBTMC_SESSION_H

#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>

#include "backends/usb/usb_transport.h"
#include "core/backend_session.h"

namespace wrvisa {

class UsbTmcBackendSession final : public BackendSession {
public:
    explicit UsbTmcBackendSession(std::unique_ptr<UsbTransport> transport)
        : transport_(std::move(transport)),
          interface_(transport_->interface_info()) {}

    ViStatus read(Operation& operation, ViPBuf buffer, ViUInt32 count,
                  ViPUInt32 return_count, ReadOptions options) override;
    ViStatus write(Operation& operation, ViConstBuf buffer, ViUInt32 count,
                   ViPUInt32 return_count, WriteOptions options) override;
    ViStatus clear() override;
    ViStatus flush(ViUInt16 mask) override;
    ViStatus read_stb(ViPUInt16 status) override;
    ViStatus assert_trigger(ViUInt16 protocol) override;
    ViStatus clear(Operation& operation) override;
    ViStatus read_stb(Operation& operation, ViPUInt16 status) override;
    ViStatus assert_trigger(Operation& operation, ViUInt16 protocol) override;
    void notify_cancel() noexcept override;
    void close() noexcept override;

private:
    std::uint8_t next_tag() noexcept;
    std::uint8_t next_status_tag() noexcept;
    void restore_read_ahead(const std::vector<std::uint8_t>& bytes);
    bool valid_interface() const noexcept;
    ViStatus ensure_capabilities(Operation& operation);
    ViStatus drain_bulk_in(Operation& operation);
    ViStatus protocol_clear(Operation& operation);
    ViStatus abort_bulk_out(Operation& operation, std::uint8_t tag);
    ViStatus abort_bulk_in(Operation& operation, std::uint8_t tag);
    bool recover_cancelled_transfer(bool bulk_in, std::uint8_t tag) noexcept;
    void mark_unusable() noexcept;

    std::unique_ptr<UsbTransport> transport_;
    UsbInterfaceInfo interface_;
    std::mutex io_mutex_;
    std::deque<std::uint8_t> read_ahead_;
    bool read_ahead_ends_message_{false};
    bool capabilities_loaded_{false};
    bool trigger_supported_{false};
    bool usable_{true};
    std::uint8_t next_tag_{1};
    std::uint8_t next_status_tag_{2};
};

}  // namespace wrvisa

#endif
