#ifndef WRVISA_RUNTIME_OPERATION_H
#define WRVISA_RUNTIME_OPERATION_H

#include <atomic>
#include <chrono>
#include <functional>
#include <mutex>

#include "visa.h"

namespace wrvisa {

class Operation {
public:
    using Clock = std::chrono::steady_clock;

    explicit Operation(ViUInt32 timeout) noexcept;

    bool try_complete(ViStatus status) noexcept;
    bool request_cancel() noexcept;
    bool request_timeout() noexcept;
    bool completed() const noexcept;
    ViStatus result() const noexcept;
    Clock::time_point deadline() const noexcept;
    void set_cancel_handler(std::function<void()> handler);
    void clear_cancel_handler() noexcept;

private:
    bool complete_and_notify(ViStatus status) noexcept;

    static constexpr ViStatus kPending = INT32_MAX;
    std::atomic<ViStatus> result_{kPending};
    Clock::time_point deadline_;
    std::mutex cancel_mutex_;
    std::function<void()> cancel_handler_;
};

}  // namespace wrvisa

#endif
