#include "backends/mock/mock_session.h"

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

namespace wrvisa {

ViStatus MockBackendSession::read(Operation& operation, ViPBuf buffer, ViUInt32 count,
                                  ViPUInt32 return_count, ReadOptions options) {
    std::unique_lock lock(mutex_);
    const auto ready = [this, &operation] {
        return operation.completed() || !incoming_.empty();
    };
    while (!ready()) {
        if (operation.deadline() == Operation::Clock::time_point::max()) {
            condition_.wait(lock, ready);
        } else if (!condition_.wait_until(lock, operation.deadline(), ready)) {
            static_cast<void>(operation.try_complete(VI_ERROR_TMO));
            break;
        }
    }
    if (operation.completed()) {
        if (return_count != nullptr) {
            *return_count = 0;
        }
        return operation.result();
    }

    const auto limit = static_cast<std::size_t>(count);
    std::size_t amount = std::min(limit, incoming_.size());
    bool found_termchar = false;
    if (options.termchar_enabled) {
        for (std::size_t index = 0; index < amount; ++index) {
            if (incoming_[index] == options.termchar) {
                amount = index + 1u;
                found_termchar = true;
                break;
            }
        }
    }

    ViStatus status = VI_SUCCESS;
    if (found_termchar) {
        status = VI_SUCCESS_TERM_CHAR;
    } else if (amount == limit) {
        status = VI_SUCCESS_MAX_CNT;
    }
    if (!operation.try_complete(status)) {
        if (return_count != nullptr) {
            *return_count = 0;
        }
        return operation.result();
    }

    for (std::size_t index = 0; index < amount; ++index) {
        buffer[index] = incoming_.front();
        incoming_.pop_front();
    }
    if (return_count != nullptr) {
        *return_count = static_cast<ViUInt32>(amount);
    }
    return status;
}

ViStatus MockBackendSession::write(Operation& operation, ViConstBuf buffer,
                                   ViUInt32 count, ViPUInt32 return_count,
                                   WriteOptions options) {
    static_cast<void>(options);
    if (!operation.try_complete(VI_SUCCESS)) {
        if (return_count != nullptr) {
            *return_count = 0;
        }
        return operation.result();
    }

    const auto size = static_cast<std::size_t>(count);
    std::string request(reinterpret_cast<const char*>(buffer), size);
    while (!request.empty() && (request.back() == '\r' || request.back() == '\n')) {
        request.pop_back();
    }
    const std::string response = request == "*IDN?"
                                     ? "WEBREAL,WRVISA-MOCK,0001,0.4\n"
                                     : std::string(reinterpret_cast<const char*>(buffer), size);
    {
        std::lock_guard lock(mutex_);
        for (const char character : response) {
            incoming_.push_back(
                static_cast<ViByte>(static_cast<unsigned char>(character)));
        }
    }
    condition_.notify_all();
    if (return_count != nullptr) {
        *return_count = count;
    }
    return VI_SUCCESS;
}

ViStatus MockBackendSession::clear() {
    std::lock_guard lock(mutex_);
    incoming_.clear();
    return VI_SUCCESS;
}

ViStatus MockBackendSession::flush(ViUInt16 mask) {
    constexpr ViUInt16 kKnownMask = VI_READ_BUF | VI_WRITE_BUF | VI_READ_BUF_DISCARD |
                                    VI_WRITE_BUF_DISCARD | VI_IO_IN_BUF |
                                    VI_IO_OUT_BUF | VI_IO_IN_BUF_DISCARD |
                                    VI_IO_OUT_BUF_DISCARD;
    if (mask == 0 || (mask & static_cast<ViUInt16>(~kKnownMask)) != 0) {
        return VI_ERROR_INV_MASK;
    }
    if ((mask & (VI_READ_BUF_DISCARD | VI_IO_IN_BUF_DISCARD)) != 0) {
        std::lock_guard lock(mutex_);
        incoming_.clear();
    }
    return VI_SUCCESS;
}

ViStatus MockBackendSession::read_stb(ViPUInt16 status) {
    if (status == nullptr) {
        return VI_ERROR_INV_PARAMETER;
    }
    *status = 0;
    return VI_SUCCESS;
}

ViStatus MockBackendSession::assert_trigger(ViUInt16 protocol) {
    return protocol == VI_TRIG_PROT_DEFAULT ? VI_SUCCESS : VI_ERROR_INV_PROT;
}

void MockBackendSession::notify_cancel() noexcept {
    // Serialize the notification with the predicate check in read().  The
    // operation state is atomic, but notifying without this mutex can still
    // lose a wake-up between the predicate check and the blocking wait.
    std::lock_guard lock(mutex_);
    condition_.notify_all();
}

}  // namespace wrvisa
