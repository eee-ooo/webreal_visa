#ifndef WRVISA_BACKENDS_GPIB_GPIB_SESSION_H
#define WRVISA_BACKENDS_GPIB_GPIB_SESSION_H

#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <vector>

#include "backends/gpib/gpib_transport.h"
#include "core/backend_session.h"

namespace wrvisa {

class GpibBackendSession final : public BackendSession {
public:
    explicit GpibBackendSession(std::unique_ptr<GpibTransport> transport)
        : transport_(std::move(transport)),
          capabilities_(transport_->capabilities()) {}

    ViStatus read(Operation& operation, ViPBuf buffer, ViUInt32 count,
                  ViPUInt32 return_count, ReadOptions options) override;
    ViStatus write(Operation& operation, ViConstBuf buffer, ViUInt32 count,
                   ViPUInt32 return_count, WriteOptions options) override;
    ViStatus clear() override;
    ViStatus clear(Operation& operation) override;
    ViStatus flush(ViUInt16 mask) override;
    ViStatus flush(Operation& operation, ViUInt16 mask) override;
    ViStatus read_stb(ViPUInt16 status) override;
    ViStatus read_stb(Operation& operation, ViPUInt16 status) override;
    ViStatus assert_trigger(ViUInt16 protocol) override;
    ViStatus assert_trigger(Operation& operation, ViUInt16 protocol) override;
    void notify_cancel() noexcept override;
    void close() noexcept override;

private:
    void restore_read_ahead(const std::vector<std::uint8_t>& data,
                            bool end);
    ViStatus flush_locked(ViUInt16 mask);

    std::unique_ptr<GpibTransport> transport_;
    GpibCapabilities capabilities_;
    // GPIB is a half-duplex bus. Keep one session transaction at a time;
    // cancel/close deliberately bypass this mutex so they can wake it.
    std::mutex io_mutex_;
    std::deque<std::uint8_t> read_ahead_;
    bool read_ahead_end_{false};
};

}  // namespace wrvisa

#endif
