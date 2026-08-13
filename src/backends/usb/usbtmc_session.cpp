#include "backends/usb/usbtmc_session.h"

#include <algorithm>
#include <cstring>
#include <limits>
#include <span>
#include <thread>
#include <vector>

#include "backends/usb/usbtmc_protocol.h"

namespace wrvisa {
namespace {

constexpr ViUInt16 kKnownFlushMask =
    VI_READ_BUF | VI_WRITE_BUF | VI_READ_BUF_DISCARD | VI_WRITE_BUF_DISCARD |
    VI_IO_IN_BUF | VI_IO_OUT_BUF | VI_IO_IN_BUF_DISCARD |
    VI_IO_OUT_BUF_DISCARD;

constexpr std::uint8_t kClassInterfaceIn = 0xa1;
constexpr std::uint8_t kClassEndpointIn = 0xa2;
constexpr std::uint8_t kInitiateAbortBulkOut = 1;
constexpr std::uint8_t kCheckAbortBulkOut = 2;
constexpr std::uint8_t kInitiateAbortBulkIn = 3;
constexpr std::uint8_t kCheckAbortBulkIn = 4;
constexpr std::uint8_t kInitiateClear = 5;
constexpr std::uint8_t kCheckClear = 6;
constexpr std::uint8_t kGetCapabilities = 7;
constexpr std::uint8_t kReadStatusByte = 128;
constexpr ViUInt32 kRecoveryTimeout = 500;

UsbControlRequest interface_request(std::uint8_t request, std::uint16_t value,
                                    const UsbInterfaceInfo& interface) {
    return UsbControlRequest{kClassInterfaceIn, request, value,
                             interface.interface_number};
}

UsbControlRequest endpoint_request(std::uint8_t request, std::uint16_t value,
                                   std::uint8_t endpoint) {
    return UsbControlRequest{kClassEndpointIn, request, value, endpoint};
}

bool operation_ready(Operation& operation) {
    if (operation.completed()) {
        return false;
    }
    if (operation.deadline() != Operation::Clock::time_point::max() &&
        operation.deadline() <= Operation::Clock::now()) {
        static_cast<void>(operation.request_timeout());
        return false;
    }
    return true;
}

}  // namespace

std::uint8_t UsbTmcBackendSession::next_tag() noexcept {
    const auto result = next_tag_;
    ++next_tag_;
    if (next_tag_ == 0) {
        next_tag_ = 1;
    }
    return result;
}

std::uint8_t UsbTmcBackendSession::next_status_tag() noexcept {
    const auto result = next_status_tag_;
    ++next_status_tag_;
    if (next_status_tag_ > 127) {
        next_status_tag_ = 2;
    }
    return result;
}

void UsbTmcBackendSession::restore_read_ahead(
    const std::vector<std::uint8_t>& bytes) {
    for (auto found = bytes.rbegin(); found != bytes.rend(); ++found) {
        read_ahead_.push_front(*found);
    }
}

bool UsbTmcBackendSession::valid_interface() const noexcept {
    return interface_.bulk_out_endpoint != 0 &&
           (interface_.bulk_out_endpoint & 0x80u) == 0 &&
           (interface_.bulk_in_endpoint & 0x80u) != 0 &&
           interface_.bulk_in_max_packet_size != 0 &&
           (interface_.interrupt_in_endpoint == 0 ||
            (interface_.interrupt_in_endpoint & 0x80u) != 0);
}

void UsbTmcBackendSession::mark_unusable() noexcept {
    usable_ = false;
    transport_->close();
}

ViStatus UsbTmcBackendSession::ensure_capabilities(Operation& operation) {
    if (capabilities_loaded_) {
        return VI_SUCCESS;
    }
    if (!valid_interface() || !interface_.usb488) {
        return VI_ERROR_NSUP_OPER;
    }
    std::vector<std::uint8_t> response;
    auto status = transport_->control_in(
        operation, interface_request(kGetCapabilities, 0, interface_), 24,
        response);
    if (status < VI_SUCCESS) {
        return status;
    }
    usbtmc::Capabilities capabilities;
    if (!usbtmc::decode_capabilities(response, true, capabilities) ||
        capabilities.status != usbtmc::kStatusSuccess ||
        !capabilities.usb488 ||
        (capabilities.usb488_service_request &&
         interface_.interrupt_in_endpoint == 0)) {
        return VI_ERROR_IO;
    }
    capabilities_loaded_ = true;
    trigger_supported_ = capabilities.usb488_trigger;
    return VI_SUCCESS;
}

ViStatus UsbTmcBackendSession::drain_bulk_in(Operation& operation) {
    if (!valid_interface()) {
        return VI_ERROR_NSUP_OPER;
    }
    while (operation_ready(operation)) {
        std::vector<std::uint8_t> discarded;
        const auto status = transport_->bulk_in(
            operation, interface_.bulk_in_max_packet_size, discarded);
        if (status < VI_SUCCESS) {
            return status;
        }
        if (discarded.size() > interface_.bulk_in_max_packet_size) {
            return VI_ERROR_IO;
        }
        if (discarded.size() < interface_.bulk_in_max_packet_size) {
            return VI_SUCCESS;
        }
    }
    return operation.result();
}

ViStatus UsbTmcBackendSession::protocol_clear(Operation& operation) {
    if (!valid_interface()) {
        return VI_ERROR_NSUP_OPER;
    }
    std::vector<std::uint8_t> response;
    auto status = transport_->control_in(
        operation, interface_request(kInitiateClear, 0, interface_), 1,
        response);
    std::uint8_t device_status = 0;
    if (status < VI_SUCCESS) {
        return status;
    }
    if (!usbtmc::decode_initiate_clear(response, device_status) ||
        device_status != usbtmc::kStatusSuccess) {
        return VI_ERROR_IO;
    }
    while (operation_ready(operation)) {
        response.clear();
        status = transport_->control_in(
            operation, interface_request(kCheckClear, 0, interface_), 2,
            response);
        if (status < VI_SUCCESS) {
            return status;
        }
        usbtmc::SplitStatus clear_status;
        if (!usbtmc::decode_check_clear(response, clear_status)) {
            return VI_ERROR_IO;
        }
        if (clear_status.status == usbtmc::kStatusSuccess) {
            return transport_->clear_halt(operation,
                                          interface_.bulk_out_endpoint);
        }
        if (clear_status.status != usbtmc::kStatusPending) {
            return VI_ERROR_IO;
        }
        if (clear_status.bulk_in_fifo_bytes) {
            status = drain_bulk_in(operation);
            if (status < VI_SUCCESS) {
                return status;
            }
        }
        std::this_thread::yield();
    }
    return operation.result();
}

ViStatus UsbTmcBackendSession::abort_bulk_out(Operation& operation,
                                               std::uint8_t tag) {
    if (!valid_interface()) {
        return VI_ERROR_NSUP_OPER;
    }
    std::vector<std::uint8_t> response;
    auto status = transport_->control_in(
        operation,
        endpoint_request(kInitiateAbortBulkOut, tag,
                         interface_.bulk_out_endpoint),
        2, response);
    std::uint8_t device_status = 0;
    if (status < VI_SUCCESS) {
        return status;
    }
    if (!usbtmc::decode_initiate_abort(response, tag, device_status) ||
        device_status != usbtmc::kStatusSuccess) {
        return VI_ERROR_IO;
    }
    while (operation_ready(operation)) {
        response.clear();
        status = transport_->control_in(
            operation,
            endpoint_request(kCheckAbortBulkOut, 0,
                             interface_.bulk_out_endpoint),
            8, response);
        if (status < VI_SUCCESS) {
            return status;
        }
        usbtmc::SplitStatus abort_status;
        if (!usbtmc::decode_check_abort_bulk_out(response, abort_status)) {
            return VI_ERROR_IO;
        }
        if (abort_status.status == usbtmc::kStatusSuccess) {
            return transport_->clear_halt(operation,
                                          interface_.bulk_out_endpoint);
        }
        if (abort_status.status != usbtmc::kStatusPending) {
            return VI_ERROR_IO;
        }
        std::this_thread::yield();
    }
    return operation.result();
}

ViStatus UsbTmcBackendSession::abort_bulk_in(Operation& operation,
                                              std::uint8_t tag) {
    if (!valid_interface()) {
        return VI_ERROR_NSUP_OPER;
    }
    std::vector<std::uint8_t> response;
    auto status = transport_->control_in(
        operation,
        endpoint_request(kInitiateAbortBulkIn, tag,
                         interface_.bulk_in_endpoint),
        2, response);
    std::uint8_t device_status = 0;
    if (status < VI_SUCCESS) {
        return status;
    }
    if (!usbtmc::decode_initiate_abort(response, tag, device_status) ||
        device_status != usbtmc::kStatusSuccess) {
        return VI_ERROR_IO;
    }
    status = drain_bulk_in(operation);
    if (status < VI_SUCCESS) {
        return status;
    }
    while (operation_ready(operation)) {
        response.clear();
        status = transport_->control_in(
            operation,
            endpoint_request(kCheckAbortBulkIn, 0,
                             interface_.bulk_in_endpoint),
            8, response);
        if (status < VI_SUCCESS) {
            return status;
        }
        usbtmc::SplitStatus abort_status;
        if (!usbtmc::decode_check_abort_bulk_in(response, abort_status)) {
            return VI_ERROR_IO;
        }
        if (abort_status.status == usbtmc::kStatusSuccess) {
            return VI_SUCCESS;
        }
        if (abort_status.status != usbtmc::kStatusPending) {
            return VI_ERROR_IO;
        }
        if (abort_status.bulk_in_fifo_bytes) {
            status = drain_bulk_in(operation);
            if (status < VI_SUCCESS) {
                return status;
            }
        }
        std::this_thread::yield();
    }
    return operation.result();
}

bool UsbTmcBackendSession::recover_cancelled_transfer(
    bool bulk_in, std::uint8_t tag) noexcept {
    try {
        Operation abort_operation(kRecoveryTimeout);
        const auto abort_status = bulk_in ? abort_bulk_in(abort_operation, tag)
                                          : abort_bulk_out(abort_operation, tag);
        if (abort_status >= VI_SUCCESS) {
            read_ahead_.clear();
            read_ahead_ends_message_ = false;
            return true;
        }
        Operation clear_operation(kRecoveryTimeout);
        if (protocol_clear(clear_operation) >= VI_SUCCESS) {
            read_ahead_.clear();
            read_ahead_ends_message_ = false;
            return true;
        }
    } catch (...) {
    }
    mark_unusable();
    return false;
}

ViStatus UsbTmcBackendSession::write(Operation& operation, ViConstBuf buffer,
                                     ViUInt32 count, ViPUInt32 return_count,
                                     WriteOptions options) {
    if (return_count != nullptr) {
        *return_count = 0;
    }
    if (buffer == nullptr || return_count == nullptr || count == 0) {
        return VI_ERROR_INV_PARAMETER;
    }
    std::lock_guard lock(io_mutex_);
    if (!usable_) {
        return VI_ERROR_CONN_LOST;
    }
    if (!valid_interface()) {
        return VI_ERROR_NSUP_OPER;
    }
    if (operation.completed()) {
        return operation.result();
    }
    const auto tag = next_tag();
    const auto frame = usbtmc::encode_dev_dep_msg_out(
        tag, std::span<const std::uint8_t>(buffer, count), options.send_end);
    if (!frame) {
        return VI_ERROR_INV_SIZE;
    }
    std::size_t transferred = 0;
    auto status = transport_->bulk_out(operation, *frame, transferred);
    if (status >= VI_SUCCESS && transferred != frame->size()) {
        status = VI_ERROR_IO;
    }
    if (status == VI_ERROR_ABORT || status == VI_ERROR_TMO) {
        if (!operation.try_complete(status)) {
            status = operation.result();
        }
        static_cast<void>(recover_cancelled_transfer(false, tag));
        return status;
    }
    if (status == VI_ERROR_CONN_LOST) {
        mark_unusable();
    }
    if (!operation.try_complete(status)) {
        return operation.result();
    }
    if (status >= VI_SUCCESS) {
        *return_count = count;
    }
    return status;
}

ViStatus UsbTmcBackendSession::read(Operation& operation, ViPBuf buffer,
                                    ViUInt32 count, ViPUInt32 return_count,
                                    ReadOptions options) {
    if (return_count != nullptr) {
        *return_count = 0;
    }
    if (buffer == nullptr || return_count == nullptr || count == 0) {
        return VI_ERROR_INV_PARAMETER;
    }

    std::lock_guard lock(io_mutex_);
    if (!usable_) {
        return VI_ERROR_CONN_LOST;
    }
    if (!valid_interface()) {
        return VI_ERROR_NSUP_OPER;
    }
    if (operation.completed()) {
        return operation.result();
    }
    std::vector<std::uint8_t> collected;
    collected.reserve(count);
    bool end_of_message = false;
    bool found_termchar = false;

    const auto consume = [&](std::span<const std::uint8_t> bytes,
                             bool message_ends) {
        bool stored_trailing = false;
        for (const auto byte : bytes) {
            if (collected.size() == count || found_termchar) {
                read_ahead_.push_back(byte);
                stored_trailing = true;
                continue;
            }
            collected.push_back(byte);
            if (options.termchar_enabled && byte == options.termchar) {
                found_termchar = true;
            }
        }
        if (message_ends) {
            if (stored_trailing) {
                read_ahead_ends_message_ = true;
            } else {
                end_of_message = true;
            }
        }
    };

    while (!read_ahead_.empty() && collected.size() < count && !found_termchar) {
        const auto byte = read_ahead_.front();
        read_ahead_.pop_front();
        collected.push_back(byte);
        if (options.termchar_enabled && byte == options.termchar) {
            found_termchar = true;
        }
        if (read_ahead_.empty() && read_ahead_ends_message_) {
            end_of_message = true;
            read_ahead_ends_message_ = false;
        }
    }

    ViStatus status = VI_SUCCESS;
    bool failed_bulk_in = false;
    bool cancellation_during_transfer = false;
    std::uint8_t active_tag = 0;
    while (collected.size() < count && !found_termchar && !end_of_message) {
        const auto remaining = count - collected.size();
        const auto tag = next_tag();
        active_tag = tag;
        const auto request = usbtmc::encode_request_dev_dep_msg_in(
            tag, static_cast<std::uint32_t>(remaining), options.termchar_enabled,
            options.termchar);
        if (!request) {
            status = VI_ERROR_INV_SIZE;
            break;
        }
        std::size_t transferred = 0;
        status = transport_->bulk_out(operation, *request, transferred);
        if (status < VI_SUCCESS || transferred != request->size()) {
            cancellation_during_transfer =
                status == VI_ERROR_ABORT || status == VI_ERROR_TMO;
            if (status >= VI_SUCCESS) {
                status = VI_ERROR_IO;
            }
            break;
        }
        failed_bulk_in = true;
        std::vector<std::uint8_t> response;
        const auto maximum_response = usbtmc::kHeaderSize + remaining +
                                      interface_.bulk_in_max_packet_size;
        status = transport_->bulk_in(operation, maximum_response, response);
        if (status < VI_SUCCESS) {
            cancellation_during_transfer =
                status == VI_ERROR_ABORT || status == VI_ERROR_TMO;
            break;
        }
        usbtmc::DevDepMessage message;
        if (response.size() == maximum_response ||
            !usbtmc::decode_dev_dep_msg_in(
                response, tag, message, remaining,
                interface_.bulk_in_max_packet_size - 1u)) {
            status = VI_ERROR_IO;
            break;
        }
        if (message.payload.empty() && !message.end_of_message) {
            status = VI_ERROR_IO;
            break;
        }
        consume(message.payload, message.end_of_message);
        failed_bulk_in = false;
    }

    if (status >= VI_SUCCESS) {
        if (found_termchar) {
            status = VI_SUCCESS_TERM_CHAR;
        } else if (collected.size() == count) {
            status = VI_SUCCESS_MAX_CNT;
        } else if (!end_of_message) {
            status = VI_ERROR_IO;
        }
    }
    if (!operation.try_complete(status)) {
        status = operation.result();
    }
    if (status == VI_ERROR_ABORT || status == VI_ERROR_TMO) {
        if (cancellation_during_transfer && active_tag != 0) {
            static_cast<void>(
                recover_cancelled_transfer(failed_bulk_in, active_tag));
        } else {
            restore_read_ahead(collected);
        }
        return status;
    }
    if (status == VI_ERROR_CONN_LOST) {
        mark_unusable();
        return status;
    }
    if (status < VI_SUCCESS) {
        restore_read_ahead(collected);
        return status;
    }
    std::memcpy(buffer, collected.data(), collected.size());
    *return_count = static_cast<ViUInt32>(collected.size());
    return status;
}

ViStatus UsbTmcBackendSession::clear() { return VI_ERROR_NSUP_OPER; }

ViStatus UsbTmcBackendSession::flush(ViUInt16 mask) {
    if (mask == 0 || (mask & static_cast<ViUInt16>(~kKnownFlushMask)) != 0) {
        return VI_ERROR_INV_MASK;
    }
    if ((mask & (VI_READ_BUF_DISCARD | VI_IO_IN_BUF_DISCARD)) != 0) {
        std::lock_guard lock(io_mutex_);
        read_ahead_.clear();
        read_ahead_ends_message_ = false;
    }
    return VI_SUCCESS;
}

ViStatus UsbTmcBackendSession::read_stb(ViPUInt16 status) {
    static_cast<void>(status);
    return VI_ERROR_NSUP_OPER;
}

ViStatus UsbTmcBackendSession::assert_trigger(ViUInt16 protocol) {
    static_cast<void>(protocol);
    return VI_ERROR_NSUP_OPER;
}

ViStatus UsbTmcBackendSession::clear(Operation& operation) {
    std::lock_guard lock(io_mutex_);
    if (!usable_) {
        return VI_ERROR_CONN_LOST;
    }
    const auto status = protocol_clear(operation);
    if (status >= VI_SUCCESS) {
        read_ahead_.clear();
        read_ahead_ends_message_ = false;
    } else if (status != VI_ERROR_NSUP_OPER) {
        mark_unusable();
    }
    return status;
}

ViStatus UsbTmcBackendSession::read_stb(Operation& operation, ViPUInt16 status) {
    if (status == nullptr) {
        return VI_ERROR_INV_PARAMETER;
    }
    std::lock_guard lock(io_mutex_);
    if (!usable_) {
        return VI_ERROR_CONN_LOST;
    }
    auto result = ensure_capabilities(operation);
    if (result < VI_SUCCESS) {
        return result;
    }
    const auto tag = next_status_tag();
    std::vector<std::uint8_t> response;
    result = transport_->control_in(
        operation, interface_request(kReadStatusByte, tag, interface_), 3,
        response);
    if (result < VI_SUCCESS) {
        return result;
    }
    std::uint8_t device_status = 0;
    std::uint8_t status_byte = 0;
    const bool has_interrupt = interface_.interrupt_in_endpoint != 0;
    if (!usbtmc::decode_read_status_byte(response, tag, has_interrupt,
                                         device_status, status_byte)) {
        return VI_ERROR_IO;
    }
    if (device_status == usbtmc::kStatusInterruptInBusy) {
        return VI_ERROR_RSRC_BUSY;
    }
    if (device_status != usbtmc::kStatusSuccess) {
        return VI_ERROR_IO;
    }
    if (!has_interrupt) {
        *status = status_byte;
        return VI_SUCCESS;
    }
    while (operation_ready(operation)) {
        response.clear();
        result = transport_->interrupt_in(operation, 2, response);
        if (result < VI_SUCCESS) {
            return result;
        }
        std::uint8_t response_tag = 0;
        if (!usbtmc::decode_interrupt_status_byte(response, response_tag,
                                                   status_byte)) {
            return VI_ERROR_IO;
        }
        if (response_tag == tag) {
            *status = status_byte;
            return VI_SUCCESS;
        }
        // Tag 1 is an asynchronous SRQ notification. Other non-matching
        // tags can be delayed responses to an earlier READ_STATUS_BYTE
        // request, so leave them consumed and keep waiting for this request.
    }
    return operation.result();
}

ViStatus UsbTmcBackendSession::assert_trigger(Operation& operation,
                                              ViUInt16 protocol) {
    if (protocol != VI_TRIG_PROT_DEFAULT) {
        return VI_ERROR_INV_PROT;
    }
    std::lock_guard lock(io_mutex_);
    if (!usable_) {
        return VI_ERROR_CONN_LOST;
    }
    auto status = ensure_capabilities(operation);
    if (status < VI_SUCCESS) {
        return status;
    }
    if (!trigger_supported_) {
        return VI_ERROR_NSUP_OPER;
    }
    const auto tag = next_tag();
    const auto frame = usbtmc::encode_usb488_trigger(tag);
    if (!frame) {
        return VI_ERROR_IO;
    }
    std::size_t transferred = 0;
    status = transport_->bulk_out(operation, *frame, transferred);
    if (status >= VI_SUCCESS && transferred != frame->size()) {
        status = VI_ERROR_IO;
    }
    if (status == VI_ERROR_ABORT || status == VI_ERROR_TMO) {
        if (!operation.try_complete(status)) {
            status = operation.result();
        }
        static_cast<void>(recover_cancelled_transfer(false, tag));
        return status;
    }
    if (status == VI_ERROR_CONN_LOST) {
        mark_unusable();
    }
    return status;
}

void UsbTmcBackendSession::notify_cancel() noexcept { transport_->cancel(); }

void UsbTmcBackendSession::close() noexcept { transport_->close(); }

}  // namespace wrvisa
