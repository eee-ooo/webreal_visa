#ifndef WRVISA_BACKENDS_VXI11_VXI11_SESSION_H
#define WRVISA_BACKENDS_VXI11_VXI11_SESSION_H

#include <atomic>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "backends/asio/request_channel.h"
#include "core/backend_session.h"

namespace wrvisa {

class Vxi11BackendSession final : public BackendSession {
public:
    static std::unique_ptr<Vxi11BackendSession> create(
        const std::string& host, const std::string& device, ViUInt32 timeout,
        ViStatus& status, std::uint16_t portmapper_port = 111);
    ~Vxi11BackendSession() override;

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
    Vxi11BackendSession(std::string host, std::string device,
                        std::uint16_t portmapper_port);

    ViStatus open(ViUInt32 timeout);
    ViStatus call(RequestChannel& channel, Operation& operation,
                  std::uint32_t program, std::uint32_t procedure,
                  const std::vector<std::uint8_t>& arguments,
                  std::vector<std::uint8_t>& result);
    ViStatus generic_call(Operation& operation, std::uint32_t procedure);
    void request_abort() noexcept;
    bool acquire_io(Operation& operation, std::unique_lock<std::timed_mutex>& lock);
    std::uint32_t next_xid() noexcept;

    std::string host_;
    std::string address_;
    std::string device_;
    std::uint16_t portmapper_port_{111};
    std::uint16_t core_port_{0};
    std::uint16_t abort_port_{0};
    std::uint32_t link_id_{0};
    std::uint32_t maximum_receive_size_{1024u * 1024u};
    std::atomic<std::uint32_t> next_xid_{1};
    std::atomic<bool> abort_requested_{false};
    std::unique_ptr<RequestChannel> core_;
    std::unique_ptr<RequestChannel> abort_;
    std::timed_mutex io_mutex_;
    std::mutex read_ahead_mutex_;
    std::deque<ViByte> read_ahead_;
    std::atomic<bool> closed_{false};
};

}  // namespace wrvisa

#endif
