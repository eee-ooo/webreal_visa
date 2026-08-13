#include "backends/usb/usb_raw_session.h"

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

bool transfer_matches(UsbTransferType type, std::uint8_t endpoint,
                      const UsbInterfaceInfo& interface, bool input) {
    if (type == UsbTransferType::none) {
        return endpoint == 0;
    }
    if (type == UsbTransferType::bulk) {
        return endpoint == (input ? interface.bulk_in_endpoint
                                  : interface.bulk_out_endpoint);
    }
    if (type == UsbTransferType::interrupt) {
        return endpoint == (input ? interface.interrupt_in_endpoint
                                  : interface.interrupt_out_endpoint);
    }
    return false;
}

}  // namespace

bool UsbRawBackendSession::valid_configuration() const noexcept {
    return interface_.interface_number <= UINT8_MAX &&
           interface_.alternate_setting == configuration_.alternate_setting &&
           transfer_matches(configuration_.read_transfer_type,
                            configuration_.read_endpoint, interface_, true) &&
           transfer_matches(configuration_.write_transfer_type,
                            configuration_.write_endpoint, interface_, false);
}

ViStatus UsbRawBackendSession::read(Operation& operation, ViPBuf buffer,
                                    ViUInt32 count, ViPUInt32 return_count,
                                    ReadOptions options) {
    *return_count = 0;
    if (!valid_configuration()) {
        return VI_ERROR_INV_PARAMETER;
    }
    if (configuration_.read_transfer_type == UsbTransferType::none) {
        return VI_ERROR_NSUP_OPER;
    }
    std::lock_guard lock(read_mutex_);
    std::vector<std::uint8_t> received;
    if (!read_ahead_.empty()) {
        const auto amount = std::min<std::size_t>(count, read_ahead_.size());
        received.reserve(amount);
        for (std::size_t index = 0; index < amount; ++index) {
            received.push_back(read_ahead_.front());
            read_ahead_.pop_front();
        }
    } else {
        ViStatus status = VI_ERROR_NSUP_OPER;
        if (configuration_.read_transfer_type == UsbTransferType::bulk) {
            status = transport_->bulk_in(operation, count, received);
        } else if (configuration_.read_transfer_type ==
                   UsbTransferType::interrupt) {
            status = transport_->interrupt_in(operation, count, received);
        }
        if (status < VI_SUCCESS) {
            return status;
        }
        if (received.size() > count) {
            return VI_ERROR_IO;
        }
    }

    ViStatus status = received.size() == count ? VI_SUCCESS_MAX_CNT
                                               : VI_SUCCESS;
    auto amount = received.size();
    auto tail = received.end();
    if (options.termchar_enabled) {
        const auto found = std::find(received.begin(), received.end(),
                                     options.termchar);
        if (found != received.end()) {
            amount = static_cast<std::size_t>(found - received.begin()) + 1u;
            tail = found + 1;
            status = VI_SUCCESS_TERM_CHAR;
        }
    }
    if (!operation.try_complete(status)) {
        read_ahead_.insert(read_ahead_.begin(), received.begin(),
                           received.end());
        return operation.result();
    }
    read_ahead_.insert(read_ahead_.end(), tail, received.end());
    if (amount != 0) {
        std::memcpy(buffer, received.data(), amount);
    }
    *return_count = static_cast<ViUInt32>(amount);
    return status;
}

ViStatus UsbRawBackendSession::write(Operation& operation, ViConstBuf buffer,
                                     ViUInt32 count,
                                     ViPUInt32 return_count,
                                     WriteOptions options) {
    static_cast<void>(options);
    *return_count = 0;
    if (!valid_configuration()) {
        return VI_ERROR_INV_PARAMETER;
    }
    if (configuration_.write_transfer_type == UsbTransferType::none) {
        return VI_ERROR_NSUP_OPER;
    }
    std::lock_guard lock(write_mutex_);
    const auto bytes = std::span<const std::uint8_t>(buffer, count);
    std::size_t transferred = 0;
    ViStatus status = VI_ERROR_NSUP_OPER;
    if (configuration_.write_transfer_type == UsbTransferType::bulk) {
        status = transport_->bulk_out(operation, bytes, transferred);
    } else if (configuration_.write_transfer_type ==
               UsbTransferType::interrupt) {
        status = transport_->interrupt_out(operation, bytes, transferred);
    }
    if (status >= VI_SUCCESS) {
        if (transferred != count) {
            return VI_ERROR_IO;
        }
        if (!operation.try_complete(status)) {
            return operation.result();
        }
        *return_count = static_cast<ViUInt32>(transferred);
    }
    return status;
}

ViStatus UsbRawBackendSession::clear() { return VI_ERROR_NSUP_OPER; }

ViStatus UsbRawBackendSession::clear(Operation& operation) {
    if (!valid_configuration()) {
        return VI_ERROR_INV_PARAMETER;
    }
    bool cleared = false;
    if (configuration_.read_endpoint != 0) {
        const auto status =
            transport_->clear_halt(operation, configuration_.read_endpoint);
        if (status < VI_SUCCESS) {
            return status;
        }
        cleared = true;
    }
    if (configuration_.write_endpoint != 0) {
        const auto status =
            transport_->clear_halt(operation, configuration_.write_endpoint);
        if (status < VI_SUCCESS) {
            return status;
        }
        cleared = true;
    }
    return cleared ? VI_SUCCESS : VI_ERROR_NSUP_OPER;
}

ViStatus UsbRawBackendSession::flush(ViUInt16 mask) {
    if (mask == 0 || (mask & static_cast<ViUInt16>(~kKnownFlushMask)) != 0) {
        return VI_ERROR_INV_MASK;
    }
    if ((mask & (VI_READ_BUF_DISCARD | VI_IO_IN_BUF_DISCARD)) != 0) {
        std::lock_guard lock(read_mutex_);
        read_ahead_.clear();
    }
    return VI_SUCCESS;
}

ViStatus UsbRawBackendSession::read_stb(ViPUInt16 status) {
    static_cast<void>(status);
    return VI_ERROR_NSUP_OPER;
}

ViStatus UsbRawBackendSession::assert_trigger(ViUInt16 protocol) {
    static_cast<void>(protocol);
    return VI_ERROR_NSUP_OPER;
}

ViStatus UsbRawBackendSession::usb_control(
    Operation& operation, std::uint8_t request_type, std::uint8_t request,
    std::uint16_t value, std::uint16_t index, ViBuf data, ViUInt32 count,
    ViPUInt32 return_count) {
    *return_count = 0;
    if (!valid_configuration()) {
        return VI_ERROR_INV_PARAMETER;
    }
    std::lock_guard lock(control_mutex_);
    const UsbControlRequest setup{request_type, request, value, index};
    if ((request_type & 0x80u) != 0) {
        std::vector<std::uint8_t> response;
        const auto status =
            transport_->control_in(operation, setup, count, response);
        if (status < VI_SUCCESS) {
            return status;
        }
        if (response.size() > count) {
            return VI_ERROR_IO;
        }
        if (!operation.try_complete(status)) {
            return operation.result();
        }
        if (!response.empty()) {
            std::memcpy(data, response.data(), response.size());
        }
        *return_count = static_cast<ViUInt32>(response.size());
        return status;
    }
    std::size_t transferred = 0;
    const auto status = transport_->control_out(
        operation, setup, std::span<const std::uint8_t>(data, count),
        transferred);
    if (status >= VI_SUCCESS) {
        if (transferred > count) {
            return VI_ERROR_IO;
        }
        if (!operation.try_complete(status)) {
            return operation.result();
        }
        *return_count = static_cast<ViUInt32>(transferred);
    }
    return status;
}

void UsbRawBackendSession::notify_cancel() noexcept { transport_->cancel(); }

void UsbRawBackendSession::close() noexcept { transport_->close(); }

}  // namespace wrvisa
