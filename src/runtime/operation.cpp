#include "runtime/operation.h"

namespace wrvisa {

Operation::Operation(ViUInt32 timeout) noexcept
    : deadline_(timeout == VI_TMO_INFINITE
                    ? Clock::time_point::max()
                    : Clock::now() + std::chrono::milliseconds(timeout)) {}

bool Operation::try_complete(ViStatus status) noexcept {
    ViStatus expected = kPending;
    return result_.compare_exchange_strong(expected, status, std::memory_order_acq_rel);
}

bool Operation::complete_and_notify(ViStatus status) noexcept {
    if (!try_complete(status)) {
        return false;
    }
    std::function<void()> handler;
    {
        std::lock_guard lock(cancel_mutex_);
        handler = cancel_handler_;
    }
    if (handler) {
        try {
            handler();
        } catch (...) {
            // Cancellation is a noexcept control path. Backends must not throw
            // from handlers, but a faulty handler must not cross the C ABI.
        }
    }
    return true;
}

bool Operation::request_cancel() noexcept {
    return complete_and_notify(VI_ERROR_ABORT);
}

bool Operation::request_timeout() noexcept {
    return complete_and_notify(VI_ERROR_TMO);
}

bool Operation::completed() const noexcept {
    return result_.load(std::memory_order_acquire) != kPending;
}

ViStatus Operation::result() const noexcept {
    return result_.load(std::memory_order_acquire);
}

Operation::Clock::time_point Operation::deadline() const noexcept { return deadline_; }

void Operation::set_cancel_handler(std::function<void()> handler) {
    std::function<void()> invoke;
    {
        std::lock_guard lock(cancel_mutex_);
        cancel_handler_ = std::move(handler);
        if (completed()) {
            invoke = cancel_handler_;
        }
    }
    if (invoke) {
        invoke();
    }
}

void Operation::clear_cancel_handler() noexcept {
    std::lock_guard lock(cancel_mutex_);
    cancel_handler_ = nullptr;
}

}  // namespace wrvisa
