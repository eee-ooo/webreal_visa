#ifndef WRVISA_BACKENDS_HISLIP_HISLIP_SESSION_H
#define WRVISA_BACKENDS_HISLIP_HISLIP_SESSION_H

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "backends/asio/request_channel.h"
#include "backends/hislip/hislip_protocol.h"
#include "core/backend_session.h"

namespace wrvisa {

class HiSlipBackendSession final : public BackendSession {
public:
    static std::unique_ptr<HiSlipBackendSession> create(
        const std::string& host, const std::string& sub_address, ViUInt32 timeout,
        ViStatus& status, std::uint16_t port = 4880);
    ~HiSlipBackendSession() override;

    ViStatus read(Operation& operation, ViPBuf buffer, ViUInt32 count,
                  ViPUInt32 return_count, ReadOptions options) override;
    ViStatus write(Operation& operation, ViConstBuf buffer, ViUInt32 count,
                   ViPUInt32 return_count, WriteOptions options) override;
    ViStatus clear() override { return VI_ERROR_NSUP_OPER; }
    ViStatus flush(ViUInt16 mask) override;
    ViStatus read_stb(ViPUInt16 status) override {
        static_cast<void>(status);
        return VI_ERROR_NSUP_OPER;
    }
    ViStatus assert_trigger(ViUInt16 protocol) override {
        static_cast<void>(protocol);
        return VI_ERROR_NSUP_OPER;
    }
    ViStatus clear(Operation& operation) override;
    ViStatus read_stb(Operation& operation, ViPUInt16 status) override;
    ViStatus assert_trigger(Operation& operation, ViUInt16 protocol) override;
    ViStatus lock(Operation& operation, ViAccessMode lock_type,
                  const std::string& access_key) override;
    ViStatus unlock(Operation& operation) override;
    ViStatus set_attribute(ViAttr attribute, ViAttrState value) override;
    ViStatus get_attribute(ViAttr attribute, void* value) override;
    void close() noexcept override;
    void notify_cancel() noexcept override;

private:
    HiSlipBackendSession(std::string host, std::string sub_address,
                         std::uint16_t port);

    ViStatus open(ViUInt32 timeout);
    ViStatus exchange(RequestChannel& channel, Operation& operation,
                      std::vector<std::uint8_t> request, hislip::Frame& response);
    ViStatus exchange_expected(RequestChannel& channel, Operation& operation,
                               std::vector<std::uint8_t> request,
                               hislip::MessageType expected,
                               hislip::Frame& response);
    void request_async_clear() noexcept;
    ViStatus complete_cancel_recovery() noexcept;
    void reset_after_clear() noexcept;
    bool acquire_io(Operation& operation, std::unique_lock<std::timed_mutex>& lock);
    std::uint32_t status_message_id() const noexcept;

    std::string host_;
    std::string address_;
    std::string sub_address_;
    std::uint16_t port_{4880};
    std::uint16_t session_id_{0};
    std::uint16_t negotiated_version_{0};
    std::uint32_t next_message_id_{UINT32_C(0xFFFFFF00)};
    std::uint32_t last_sent_message_id_{UINT32_C(0xFFFFFEFE)};
    std::size_t maximum_outgoing_payload_{16u * 1024u * 1024u};
    std::unique_ptr<RequestChannel> synchronous_;
    std::unique_ptr<RequestChannel> asynchronous_;
    std::timed_mutex io_mutex_;
    std::mutex read_ahead_mutex_;
    std::deque<ViByte> read_ahead_;
    bool read_ahead_end_{false};
    std::atomic<ViUInt16> cached_status_{0};
    std::atomic<bool> remote_locked_{false};
    std::atomic<bool> clear_requested_{false};
    std::atomic<bool> clear_acknowledged_{false};
    std::atomic<bool> clear_request_finished_{false};
    std::mutex clear_mutex_;
    std::condition_variable clear_condition_;
    std::atomic<bool> closed_{false};
};

}  // namespace wrvisa

#endif
