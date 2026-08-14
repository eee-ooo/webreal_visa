#include "backends/gpib/gpib_session.h"

#include <algorithm>
#include <cstring>
#include <span>
#include <vector>

namespace wrvisa {
namespace {

constexpr ViUInt16 kKnownFlushMask =
    VI_READ_BUF | VI_WRITE_BUF | VI_READ_BUF_DISCARD |
    VI_WRITE_BUF_DISCARD | VI_IO_IN_BUF | VI_IO_OUT_BUF |
    VI_IO_IN_BUF_DISCARD | VI_IO_OUT_BUF_DISCARD;

}  // namespace

void GpibBackendSession::restore_read_ahead(
    const std::vector<std::uint8_t>& data, bool end) {
    read_ahead_.insert(read_ahead_.begin(), data.begin(), data.end());
    if (end) {
        read_ahead_end_ = true;
    }
}

ViStatus GpibBackendSession::read(Operation& operation, ViPBuf buffer,
                                  ViUInt32 count, ViPUInt32 return_count,
                                  ReadOptions options) {
    *return_count = 0;
    std::lock_guard lock(io_mutex_);
    if (operation.completed()) {
        return operation.result();
    }
    std::vector<std::uint8_t> received;
    received.reserve(count);
    bool end = false;
    std::size_t amount = 0;
    ViStatus completion_status = VI_SUCCESS;

    while (received.size() < count && !end) {
        if (read_ahead_.empty() && read_ahead_end_) {
            read_ahead_end_ = false;
            end = true;
            break;
        }

        if (!read_ahead_.empty()) {
            const auto remaining = static_cast<std::size_t>(count) - received.size();
            const auto take = std::min(remaining, read_ahead_.size());
            for (std::size_t index = 0; index < take; ++index) {
                received.push_back(read_ahead_.front());
                read_ahead_.pop_front();
            }
            if (read_ahead_.empty() && read_ahead_end_) {
                read_ahead_end_ = false;
                end = true;
            }
        } else {
            std::vector<std::uint8_t> chunk;
            bool chunk_end = false;
            const auto maximum = static_cast<std::size_t>(count) - received.size();
            const auto status =
                transport_->read(operation, maximum, chunk, chunk_end);
            if (status < VI_SUCCESS) {
                restore_read_ahead(received, end);
                return status;
            }
            if (chunk.size() > maximum || (chunk.empty() && !chunk_end)) {
                restore_read_ahead(received, end);
                return VI_ERROR_IO;
            }
            received.insert(received.end(), chunk.begin(), chunk.end());
            end = chunk_end;
        }

        if (options.termchar_enabled) {
            const auto found = std::find(received.begin(), received.end(),
                                         options.termchar);
            if (found != received.end()) {
                amount = static_cast<std::size_t>(found - received.begin()) + 1u;
                completion_status = VI_SUCCESS_TERM_CHAR;
                break;
            }
        }
    }

    if (completion_status != VI_SUCCESS_TERM_CHAR) {
        amount = received.size();
        completion_status = amount == count ? VI_SUCCESS_MAX_CNT : VI_SUCCESS;
    }
    if (!operation.try_complete(completion_status)) {
        restore_read_ahead(received, end);
        return operation.result();
    }

    if (amount < received.size()) {
        read_ahead_.insert(read_ahead_.begin(), received.begin() +
                                                   static_cast<std::ptrdiff_t>(amount),
                           received.end());
        if (end) {
            read_ahead_end_ = true;
        }
    }
    if (amount != 0) {
        std::memcpy(buffer, received.data(), amount);
    }
    *return_count = static_cast<ViUInt32>(amount);
    return completion_status;
}

ViStatus GpibBackendSession::write(Operation& operation, ViConstBuf buffer,
                                   ViUInt32 count,
                                   ViPUInt32 return_count,
                                   WriteOptions options) {
    *return_count = 0;
    if (options.send_end && !capabilities_.send_end) {
        return VI_ERROR_NSUP_OPER;
    }
    std::lock_guard lock(io_mutex_);
    if (operation.completed()) {
        return operation.result();
    }
    std::size_t transferred = 0;
    const auto status = transport_->write(
        operation, std::span<const std::uint8_t>(buffer, count),
        options.send_end, transferred);
    if (status < VI_SUCCESS) {
        return status;
    }
    if (transferred != count) {
        return VI_ERROR_IO;
    }
    if (!operation.try_complete(status)) {
        return operation.result();
    }
    *return_count = static_cast<ViUInt32>(transferred);
    return status;
}

ViStatus GpibBackendSession::clear() { return VI_ERROR_NSUP_OPER; }

ViStatus GpibBackendSession::clear(Operation& operation) {
    if (!capabilities_.device_clear) {
        return VI_ERROR_NSUP_OPER;
    }
    std::lock_guard lock(io_mutex_);
    if (operation.completed()) {
        return operation.result();
    }
    const auto status = transport_->clear(operation);
    if (status >= VI_SUCCESS) {
        read_ahead_.clear();
        read_ahead_end_ = false;
    }
    return status;
}

ViStatus GpibBackendSession::flush_locked(ViUInt16 mask) {
    if (mask == 0 || (mask & static_cast<ViUInt16>(~kKnownFlushMask)) != 0) {
        return VI_ERROR_INV_MASK;
    }
    if ((mask & (VI_READ_BUF_DISCARD | VI_IO_IN_BUF_DISCARD)) != 0) {
        read_ahead_.clear();
        read_ahead_end_ = false;
    }
    return VI_SUCCESS;
}

ViStatus GpibBackendSession::flush(ViUInt16 mask) {
    std::lock_guard lock(io_mutex_);
    return flush_locked(mask);
}

ViStatus GpibBackendSession::flush(Operation& operation, ViUInt16 mask) {
    if (mask == 0 || (mask & static_cast<ViUInt16>(~kKnownFlushMask)) != 0) {
        return VI_ERROR_INV_MASK;
    }
    std::lock_guard lock(io_mutex_);
    if (operation.completed()) {
        return operation.result();
    }
    return flush_locked(mask);
}

ViStatus GpibBackendSession::read_stb(ViPUInt16 status) {
    static_cast<void>(status);
    return VI_ERROR_NSUP_OPER;
}

ViStatus GpibBackendSession::read_stb(Operation& operation,
                                      ViPUInt16 status) {
    if (status == nullptr) {
        return VI_ERROR_INV_PARAMETER;
    }
    if (!capabilities_.serial_poll) {
        return VI_ERROR_NSUP_OPER;
    }
    std::lock_guard lock(io_mutex_);
    if (operation.completed()) {
        return operation.result();
    }
    std::uint8_t value = 0;
    const auto result = transport_->serial_poll(operation, value);
    if (result < VI_SUCCESS) {
        return result;
    }
    if (!operation.try_complete(result)) {
        return operation.result();
    }
    *status = value;
    return result;
}

ViStatus GpibBackendSession::assert_trigger(ViUInt16 protocol) {
    static_cast<void>(protocol);
    return VI_ERROR_NSUP_OPER;
}

ViStatus GpibBackendSession::assert_trigger(Operation& operation,
                                            ViUInt16 protocol) {
    if (protocol != VI_TRIG_PROT_DEFAULT) {
        return VI_ERROR_INV_PROT;
    }
    if (!capabilities_.trigger) {
        return VI_ERROR_NSUP_OPER;
    }
    std::lock_guard lock(io_mutex_);
    if (operation.completed()) {
        return operation.result();
    }
    return transport_->trigger(operation);
}

void GpibBackendSession::notify_cancel() noexcept { transport_->cancel(); }

void GpibBackendSession::close() noexcept { transport_->close(); }

}  // namespace wrvisa
