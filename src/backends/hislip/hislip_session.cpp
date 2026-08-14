#include "backends/hislip/hislip_session.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <limits>
#include <span>
#include <string_view>
#include <utility>

namespace wrvisa {
namespace {

constexpr std::size_t kMaximumPayload = 16u * 1024u * 1024u;
constexpr std::uint16_t kClientProtocolVersion = UINT16_C(0x0100);
constexpr std::uint16_t kClientVendorId = UINT16_C(0x5752);  // "WR"

ViUInt32 remaining_timeout(const Operation& operation) noexcept {
    if (operation.deadline() == Operation::Clock::time_point::max()) {
        return VI_TMO_INFINITE;
    }
    const auto now = Operation::Clock::now();
    if (operation.deadline() <= now) {
        return 0;
    }
    const auto value = std::chrono::duration_cast<std::chrono::milliseconds>(
        operation.deadline() - now);
    const auto count = value.count();
    if (count >= static_cast<std::int64_t>(UINT32_MAX)) {
        return UINT32_MAX - 1u;
    }
    return static_cast<ViUInt32>(std::max<std::int64_t>(count, 1));
}

void append_u64(std::vector<std::uint8_t>& output, std::uint64_t value) {
    for (int shift = 56; shift >= 0; shift -= 8) {
        output.push_back(static_cast<std::uint8_t>(
            value >> static_cast<unsigned>(shift)));
    }
}

std::uint64_t read_u64(const std::uint8_t* input) noexcept {
    std::uint64_t value = 0;
    for (std::size_t index = 0; index < 8u; ++index) {
        value = (value << 8u) | input[index];
    }
    return value;
}

void copy_string(void* destination, std::string_view value) {
    const auto amount = std::min<std::size_t>(value.size(), VI_FIND_BUFLEN - 1u);
    std::memcpy(destination, value.data(), amount);
    static_cast<ViChar*>(destination)[amount] = '\0';
}

bool valid_flush_mask(ViUInt16 mask) noexcept {
    constexpr ViUInt16 known = VI_READ_BUF | VI_WRITE_BUF |
                               VI_READ_BUF_DISCARD | VI_WRITE_BUF_DISCARD |
                               VI_IO_IN_BUF | VI_IO_OUT_BUF |
                               VI_IO_IN_BUF_DISCARD | VI_IO_OUT_BUF_DISCARD;
    return mask != 0 && (mask & static_cast<ViUInt16>(~known)) == 0;
}

}  // namespace

HiSlipBackendSession::HiSlipBackendSession(std::string host,
                                           std::string sub_address,
                                           std::uint16_t port)
    : host_(std::move(host)), sub_address_(std::move(sub_address)), port_(port),
      synchronous_(std::make_unique<RequestChannel>(kMaximumPayload, true)),
      asynchronous_(std::make_unique<RequestChannel>(kMaximumPayload)) {}

HiSlipBackendSession::~HiSlipBackendSession() { close(); }

std::unique_ptr<HiSlipBackendSession> HiSlipBackendSession::create(
    const std::string& host, const std::string& sub_address, ViUInt32 timeout,
    ViStatus& status, std::uint16_t port) {
    auto session = std::unique_ptr<HiSlipBackendSession>(
        new HiSlipBackendSession(host, sub_address, port));
    status = session->open(timeout);
    if (status < VI_SUCCESS) {
        return nullptr;
    }
    return session;
}

ViStatus HiSlipBackendSession::exchange(RequestChannel& channel,
                                        Operation& operation,
                                        std::vector<std::uint8_t> request,
                                        hislip::Frame& response) {
    const auto* channel_name = &channel == synchronous_.get() ? "sync" : "async";
    std::vector<std::uint8_t> bytes;
    const auto status = channel.exchange(operation, std::move(request),
                                         ResponseFraming::hislip_frame, bytes);
    if (status < VI_SUCCESS) {
        if (status == VI_ERROR_IO) {
            std::fprintf(stderr,
                         "HiSLIP trace: %s RequestChannel returned VI_ERROR_IO "
                         "(operation completed=%d result=%d)\n",
                         channel_name, operation.completed() ? 1 : 0,
                         static_cast<int>(operation.result()));
        }
        return status;
    }
    if (!hislip::decode(bytes, response)) {
        std::fprintf(stderr,
                     "HiSLIP trace: %s response decode failed (%zu bytes)\n",
                     channel_name, bytes.size());
        close();
        return VI_ERROR_IO;
    }
    if (response.type == hislip::MessageType::fatal_error) {
        close();
        return VI_ERROR_CONN_LOST;
    }
    if (response.type == hislip::MessageType::error) {
        std::fprintf(stderr, "HiSLIP trace: %s received Error frame\n",
                     channel_name);
        return VI_ERROR_IO;
    }
    return VI_SUCCESS;
}

ViStatus HiSlipBackendSession::exchange_expected(
    RequestChannel& channel, Operation& operation,
    std::vector<std::uint8_t> request, hislip::MessageType expected,
    hislip::Frame& response) {
    for (;;) {
        const auto status = exchange(channel, operation, std::move(request), response);
        request.clear();
        if (status < VI_SUCCESS) {
            return status;
        }
        if (response.type == expected) {
            return VI_SUCCESS;
        }
        if (response.type == hislip::MessageType::async_service_request) {
            cached_status_.store(response.control, std::memory_order_release);
            continue;
        }
        if (response.type == hislip::MessageType::data ||
            response.type == hislip::MessageType::data_end ||
            response.type == hislip::MessageType::interrupted ||
            response.type == hislip::MessageType::async_interrupted) {
            continue;
        }
        std::fprintf(stderr,
                     "HiSLIP trace: exchange_expected got type=%u, expected=%u\n",
                     static_cast<unsigned>(response.type),
                     static_cast<unsigned>(expected));
        return VI_ERROR_IO;
    }
}

ViStatus HiSlipBackendSession::open(ViUInt32 timeout) {
    Operation operation(timeout);
    auto status = synchronous_->connect(host_, port_, remaining_timeout(operation),
                                        address_);
    if (status < VI_SUCCESS) {
        return status;
    }
    const auto* sub_address_bytes =
        reinterpret_cast<const std::uint8_t*>(sub_address_.data());
    auto initialize = hislip::encode(
        hislip::MessageType::initialize, 0,
        (static_cast<std::uint32_t>(kClientProtocolVersion) << 16u) |
            kClientVendorId,
        std::span<const std::uint8_t>(sub_address_bytes, sub_address_.size()));
    hislip::Frame response;
    status = exchange_expected(*synchronous_, operation, std::move(initialize),
                               hislip::MessageType::initialize_response, response);
    if (status < VI_SUCCESS) {
        return status;
    }
    negotiated_version_ = static_cast<std::uint16_t>(response.parameter >> 16u);
    session_id_ = static_cast<std::uint16_t>(response.parameter);
    if (negotiated_version_ == 0 || negotiated_version_ > kClientProtocolVersion ||
        (response.control & 0x07u) != 0u) {
        return VI_ERROR_NSUP_OPER;
    }
    std::string ignored_address;
    status = asynchronous_->connect(host_, port_, remaining_timeout(operation),
                                    ignored_address);
    if (status < VI_SUCCESS) {
        return status;
    }
    auto async_initialize = hislip::encode(
        hislip::MessageType::async_initialize, 0, session_id_);
    status = exchange_expected(*asynchronous_, operation,
                               std::move(async_initialize),
                               hislip::MessageType::async_initialize_response,
                               response);
    if (status < VI_SUCCESS) {
        return status;
    }

    std::vector<std::uint8_t> maximum_payload;
    append_u64(maximum_payload, kMaximumPayload);
    auto maximum_message = hislip::encode(
        hislip::MessageType::async_maximum_message_size, 0, 0, maximum_payload);
    status = exchange_expected(
        *asynchronous_, operation, std::move(maximum_message),
        hislip::MessageType::async_maximum_message_size_response, response);
    if (status < VI_SUCCESS || response.payload.size() != 8u) {
        return status < VI_SUCCESS ? status : VI_ERROR_IO;
    }
    const auto peer_maximum = read_u64(response.payload.data());
    if (peer_maximum == 0) {
        return VI_ERROR_IO;
    }
    maximum_outgoing_payload_ = static_cast<std::size_t>(
        std::min<std::uint64_t>(peer_maximum, kMaximumPayload));
    synchronous_->set_cancel_observer([this] { request_async_clear(); });
    return VI_SUCCESS;
}

void HiSlipBackendSession::request_async_clear() noexcept {
    if (closed_.load(std::memory_order_acquire) ||
        clear_requested_.exchange(true, std::memory_order_acq_rel)) {
        return;
    }
    bool acknowledged = false;
    try {
        {
            std::lock_guard lock(clear_mutex_);
            clear_acknowledged_ = false;
            clear_request_finished_ = false;
        }
        Operation operation(500);
        hislip::Frame response;
        auto request = hislip::encode(hislip::MessageType::async_device_clear, 0, 0);
        const auto status = exchange_expected(
            *asynchronous_, operation, std::move(request),
            hislip::MessageType::async_device_clear_acknowledge, response);
        acknowledged = status >= VI_SUCCESS;
    } catch (...) {
        acknowledged = false;
    }
    {
        std::lock_guard lock(clear_mutex_);
        clear_acknowledged_ = acknowledged;
        clear_request_finished_ = true;
    }
    clear_condition_.notify_all();
}

void HiSlipBackendSession::reset_after_clear() noexcept {
    next_message_id_ = UINT32_C(0xFFFFFF00);
    last_sent_message_id_ = UINT32_C(0xFFFFFEFE);
    clear_requested_.store(false, std::memory_order_release);
    {
        std::lock_guard clear_lock(clear_mutex_);
        clear_acknowledged_ = false;
        clear_request_finished_ = false;
    }
    std::lock_guard ahead_lock(read_ahead_mutex_);
    read_ahead_.clear();
    read_ahead_end_ = false;
}

ViStatus HiSlipBackendSession::complete_cancel_recovery() noexcept {
    {
        std::unique_lock lock(clear_mutex_);
        const auto finished = clear_condition_.wait_for(
            lock, std::chrono::milliseconds(500),
            [this] { return clear_request_finished_; });
        if (!finished || !clear_acknowledged_) {
            return VI_ERROR_CONN_LOST;
        }
    }
    try {
        Operation operation(500);
        hislip::Frame response;
        auto request = hislip::encode(hislip::MessageType::device_clear_complete, 0,
                                      0);
        const auto status = exchange_expected(
            *synchronous_, operation, std::move(request),
            hislip::MessageType::device_clear_acknowledge, response);
        if (status >= VI_SUCCESS) {
            reset_after_clear();
        }
        return status;
    } catch (...) {
        return VI_ERROR_CONN_LOST;
    }
}

bool HiSlipBackendSession::acquire_io(
    Operation& operation, std::unique_lock<std::timed_mutex>& lock) {
    while (!operation.completed()) {
        if (operation.deadline() != Operation::Clock::time_point::max() &&
            operation.deadline() <= Operation::Clock::now()) {
            static_cast<void>(operation.request_timeout());
            break;
        }
        if (lock.try_lock_for(std::chrono::milliseconds(5))) {
            return true;
        }
    }
    return false;
}

std::uint32_t HiSlipBackendSession::status_message_id() const noexcept {
    return last_sent_message_id_;
}

ViStatus HiSlipBackendSession::write(Operation& operation, ViConstBuf buffer,
                                     ViUInt32 count, ViPUInt32 return_count,
                                     WriteOptions options) {
    *return_count = 0;
    std::unique_lock<std::timed_mutex> lock(io_mutex_, std::defer_lock);
    if (!acquire_io(operation, lock)) {
        return operation.result();
    }
    {
        std::lock_guard ahead_lock(read_ahead_mutex_);
        read_ahead_.clear();
        read_ahead_end_ = false;
    }
    std::size_t offset = 0;
    while (offset < count) {
        const auto amount = std::min<std::size_t>(
            static_cast<std::size_t>(count) - offset, maximum_outgoing_payload_);
        const bool last = offset + amount == count;
        const auto type = last && options.send_end
                              ? hislip::MessageType::data_end
                              : hislip::MessageType::data;
        const auto message_id = next_message_id_;
        next_message_id_ += 2u;
        last_sent_message_id_ = message_id;
        auto frame = hislip::encode(
            type, 0, message_id,
            std::span<const std::uint8_t>(buffer + offset, amount));
        std::vector<std::uint8_t> ignored;
        const auto status = synchronous_->exchange(
            operation, std::move(frame), ResponseFraming::none, ignored);
        if (status < VI_SUCCESS) {
            if ((status == VI_ERROR_ABORT || status == VI_ERROR_TMO) &&
                complete_cancel_recovery() < VI_SUCCESS) {
                return VI_ERROR_CONN_LOST;
            }
            return status;
        }
        offset += amount;
    }
    if (!operation.try_complete(VI_SUCCESS)) {
        return operation.result();
    }
    *return_count = count;
    return VI_SUCCESS;
}

ViStatus HiSlipBackendSession::read(Operation& operation, ViPBuf buffer,
                                    ViUInt32 count, ViPUInt32 return_count,
                                    ReadOptions options) {
    *return_count = 0;
    std::unique_lock<std::timed_mutex> lock(io_mutex_, std::defer_lock);
    if (!acquire_io(operation, lock)) {
        return operation.result();
    }
    std::vector<ViByte> data;
    data.reserve(std::min<std::size_t>(count, 4096u));
    bool end = false;
    bool termchar = false;
    {
        std::lock_guard ahead_lock(read_ahead_mutex_);
        while (!read_ahead_.empty() && data.size() < count) {
            const auto byte = read_ahead_.front();
            read_ahead_.pop_front();
            data.push_back(byte);
            if (options.termchar_enabled && byte == options.termchar) {
                termchar = true;
                break;
            }
        }
        if (read_ahead_.empty() && read_ahead_end_) {
            end = true;
            read_ahead_end_ = false;
        }
    }
    while (!end && !termchar && data.size() < count) {
        hislip::Frame frame;
        auto status = exchange(*synchronous_, operation, {}, frame);
        if (status < VI_SUCCESS) {
            {
                std::lock_guard ahead_lock(read_ahead_mutex_);
                for (auto iterator = data.rbegin(); iterator != data.rend();
                     ++iterator) {
                    read_ahead_.push_front(*iterator);
                }
            }
            if ((status == VI_ERROR_ABORT || status == VI_ERROR_TMO) &&
                complete_cancel_recovery() < VI_SUCCESS) {
                return VI_ERROR_CONN_LOST;
            }
            return status;
        }
        if (frame.type == hislip::MessageType::interrupted) {
            std::fprintf(stderr,
                         "HiSLIP trace: read received Interrupted as success "
                         "(operation completed=%d result=%d)\n",
                         operation.completed() ? 1 : 0,
                         static_cast<int>(operation.result()));
            close();
            return VI_ERROR_IO;
        }
        if (frame.type != hislip::MessageType::data &&
            frame.type != hislip::MessageType::data_end) {
            std::fprintf(stderr,
                         "HiSLIP trace: read received unexpected type=%u\n",
                         static_cast<unsigned>(frame.type));
            close();
            return VI_ERROR_IO;
        }
        if (frame.parameter != UINT32_MAX &&
            frame.parameter != last_sent_message_id_) {
            std::lock_guard ahead_lock(read_ahead_mutex_);
            read_ahead_.clear();
            read_ahead_end_ = false;
            std::fprintf(stderr,
                         "HiSLIP trace: read message id=%u, expected=%u\n",
                         static_cast<unsigned>(frame.parameter),
                         static_cast<unsigned>(last_sent_message_id_));
            close();
            return VI_ERROR_IO;
        }
        for (std::size_t index = 0; index < frame.payload.size(); ++index) {
            const auto byte = frame.payload[index];
            if (data.size() < count && !termchar) {
                data.push_back(byte);
                if (options.termchar_enabled && byte == options.termchar) {
                    termchar = true;
                }
            } else {
                std::lock_guard ahead_lock(read_ahead_mutex_);
                read_ahead_.push_back(byte);
            }
        }
        if (frame.type == hislip::MessageType::data_end) {
            std::lock_guard ahead_lock(read_ahead_mutex_);
            if (read_ahead_.empty()) {
                end = true;
            } else {
                read_ahead_end_ = true;
            }
        }
    }
    const auto status = termchar
                            ? VI_SUCCESS_TERM_CHAR
                            : (data.size() == count ? VI_SUCCESS_MAX_CNT : VI_SUCCESS);
    if (!operation.try_complete(status)) {
        std::lock_guard ahead_lock(read_ahead_mutex_);
        for (auto iterator = data.rbegin(); iterator != data.rend(); ++iterator) {
            read_ahead_.push_front(*iterator);
        }
        return operation.result();
    }
    std::memcpy(buffer, data.data(), data.size());
    *return_count = static_cast<ViUInt32>(data.size());
    return status;
}

ViStatus HiSlipBackendSession::clear(Operation& operation) {
    std::unique_lock<std::timed_mutex> lock(io_mutex_, std::defer_lock);
    if (!acquire_io(operation, lock)) {
        return operation.result();
    }
    hislip::Frame response;
    auto request = hislip::encode(hislip::MessageType::async_device_clear, 0, 0);
    auto status = exchange_expected(
        *asynchronous_, operation, std::move(request),
        hislip::MessageType::async_device_clear_acknowledge, response);
    if (status < VI_SUCCESS) {
        return status;
    }
    request = hislip::encode(hislip::MessageType::device_clear_complete, 0, 0);
    status = exchange_expected(*synchronous_, operation, std::move(request),
                               hislip::MessageType::device_clear_acknowledge,
                               response);
    if (status >= VI_SUCCESS) {
        reset_after_clear();
    }
    return status;
}

ViStatus HiSlipBackendSession::read_stb(Operation& operation,
                                        ViPUInt16 status_byte) {
    if (status_byte == nullptr) {
        return VI_ERROR_INV_PARAMETER;
    }
    std::unique_lock<std::timed_mutex> lock(io_mutex_, std::defer_lock);
    if (!acquire_io(operation, lock)) {
        return operation.result();
    }
    hislip::Frame response;
    auto request = hislip::encode(hislip::MessageType::async_status_query, 0,
                                  status_message_id());
    const auto status = exchange_expected(
        *asynchronous_, operation, std::move(request),
        hislip::MessageType::async_status_response, response);
    if (status >= VI_SUCCESS) {
        cached_status_.store(response.control, std::memory_order_release);
        *status_byte = response.control;
    }
    return status;
}

ViStatus HiSlipBackendSession::assert_trigger(Operation& operation,
                                              ViUInt16 protocol) {
    if (protocol != VI_TRIG_PROT_DEFAULT) {
        return VI_ERROR_INV_PROT;
    }
    std::unique_lock<std::timed_mutex> lock(io_mutex_, std::defer_lock);
    if (!acquire_io(operation, lock)) {
        return operation.result();
    }
    const auto message_id = next_message_id_;
    next_message_id_ += 2u;
    last_sent_message_id_ = message_id;
    auto request = hislip::encode(hislip::MessageType::trigger, 0, message_id);
    std::vector<std::uint8_t> ignored;
    const auto status = synchronous_->exchange(operation, std::move(request),
                                               ResponseFraming::none, ignored);
    if ((status == VI_ERROR_ABORT || status == VI_ERROR_TMO) &&
        complete_cancel_recovery() < VI_SUCCESS) {
        return VI_ERROR_CONN_LOST;
    }
    return status;
}

ViStatus HiSlipBackendSession::lock(Operation& operation, ViAccessMode lock_type,
                                    const std::string& access_key) {
    std::unique_lock<std::timed_mutex> lock(io_mutex_, std::defer_lock);
    if (!acquire_io(operation, lock)) {
        return operation.result();
    }
    std::span<const std::uint8_t> lock_string;
    if (lock_type == VI_SHARED_LOCK) {
        lock_string = std::span<const std::uint8_t>(
            reinterpret_cast<const std::uint8_t*>(access_key.data()),
            access_key.size());
    }
    auto request = hislip::encode(hislip::MessageType::async_lock, 1,
                                  remaining_timeout(operation), lock_string);
    hislip::Frame response;
    const auto status = exchange_expected(
        *asynchronous_, operation, std::move(request),
        hislip::MessageType::async_lock_response, response);
    if (status < VI_SUCCESS) {
        return status;
    }
    if (response.control == 0) {
        return VI_ERROR_TMO;
    }
    if (response.control != 1) {
        return VI_ERROR_RSRC_LOCKED;
    }
    remote_locked_.store(true, std::memory_order_release);
    return VI_SUCCESS;
}

ViStatus HiSlipBackendSession::unlock(Operation& operation) {
    if (!remote_locked_.load(std::memory_order_acquire)) {
        return VI_SUCCESS;
    }
    std::unique_lock<std::timed_mutex> lock(io_mutex_, std::defer_lock);
    if (!acquire_io(operation, lock)) {
        return operation.result();
    }
    auto request = hislip::encode(hislip::MessageType::async_lock, 0,
                                  status_message_id());
    hislip::Frame response;
    const auto status = exchange_expected(
        *asynchronous_, operation, std::move(request),
        hislip::MessageType::async_lock_response, response);
    if (status < VI_SUCCESS) {
        return status;
    }
    if (response.control != 1 && response.control != 2) {
        return VI_ERROR_SESN_NLOCKED;
    }
    remote_locked_.store(false, std::memory_order_release);
    return VI_SUCCESS;
}

ViStatus HiSlipBackendSession::flush(ViUInt16 mask) {
    if (!valid_flush_mask(mask)) {
        return VI_ERROR_INV_MASK;
    }
    if ((mask & (VI_READ_BUF_DISCARD | VI_IO_IN_BUF_DISCARD)) != 0) {
        std::lock_guard lock(read_ahead_mutex_);
        read_ahead_.clear();
        read_ahead_end_ = false;
    }
    return VI_SUCCESS;
}

ViStatus HiSlipBackendSession::set_attribute(ViAttr attribute,
                                             ViAttrState value) {
    static_cast<void>(value);
    if (attribute == VI_ATTR_TCPIP_ADDR || attribute == VI_ATTR_TCPIP_HOSTNAME ||
        attribute == VI_ATTR_TCPIP_PORT) {
        return VI_ERROR_ATTR_READONLY;
    }
    return VI_ERROR_NSUP_ATTR;
}

ViStatus HiSlipBackendSession::get_attribute(ViAttr attribute, void* value) {
    switch (attribute) {
        case VI_ATTR_TCPIP_ADDR:
            copy_string(value, address_);
            return VI_SUCCESS;
        case VI_ATTR_TCPIP_HOSTNAME:
            copy_string(value, host_);
            return VI_SUCCESS;
        case VI_ATTR_TCPIP_PORT:
            *static_cast<ViUInt16*>(value) = port_;
            return VI_SUCCESS;
        default:
            return VI_ERROR_NSUP_ATTR;
    }
}

void HiSlipBackendSession::notify_cancel() noexcept {
    request_async_clear();
}

void HiSlipBackendSession::close() noexcept {
    if (closed_.exchange(true, std::memory_order_acq_rel)) {
        return;
    }
    if (asynchronous_) {
        asynchronous_->close();
    }
    if (synchronous_) {
        synchronous_->close();
    }
}

}  // namespace wrvisa
