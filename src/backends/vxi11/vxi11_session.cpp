#include "backends/vxi11/vxi11_session.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <limits>
#include <span>
#include <string_view>
#include <utility>

#include "backends/vxi11/vxi11_protocol.h"

namespace wrvisa {
namespace {

constexpr std::size_t kMaximumRpcPayload = 16u * 1024u * 1024u;

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

ViStatus map_device_error(std::uint32_t error) noexcept {
    switch (error) {
        case 0:
            return VI_SUCCESS;
        case 3:
        case 4:
        case 6:
            return VI_ERROR_CONN_LOST;
        case 5:
        case 21:
            return VI_ERROR_INV_PARAMETER;
        case 8:
            return VI_ERROR_NSUP_OPER;
        case 9:
            return VI_ERROR_ALLOC;
        case 11:
            return VI_ERROR_RSRC_LOCKED;
        case 12:
            return VI_ERROR_SESN_NLOCKED;
        case 15:
            return VI_ERROR_TMO;
        case 17:
            return VI_ERROR_IO;
        case 23:
            return VI_ERROR_ABORT;
        case 29:
            return VI_ERROR_RSRC_BUSY;
        default:
            return VI_ERROR_IO;
    }
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

Vxi11BackendSession::Vxi11BackendSession(std::string host, std::string device,
                                         std::uint16_t portmapper_port)
    : host_(std::move(host)), device_(std::move(device)),
      portmapper_port_(portmapper_port),
      core_(std::make_unique<RequestChannel>(kMaximumRpcPayload, true)),
      abort_(std::make_unique<RequestChannel>(kMaximumRpcPayload)) {}

Vxi11BackendSession::~Vxi11BackendSession() { close(); }

std::unique_ptr<Vxi11BackendSession> Vxi11BackendSession::create(
    const std::string& host, const std::string& device, ViUInt32 timeout,
    ViStatus& status, std::uint16_t portmapper_port) {
    auto session = std::unique_ptr<Vxi11BackendSession>(
        new Vxi11BackendSession(host, device, portmapper_port));
    status = session->open(timeout);
    if (status < VI_SUCCESS) {
        return nullptr;
    }
    return session;
}

std::uint32_t Vxi11BackendSession::next_xid() noexcept {
    return next_xid_.fetch_add(1u, std::memory_order_relaxed);
}

ViStatus Vxi11BackendSession::call(RequestChannel& channel, Operation& operation,
                                   std::uint32_t program,
                                   std::uint32_t procedure,
                                   const std::vector<std::uint8_t>& arguments,
                                   std::vector<std::uint8_t>& result) {
    const auto xid = next_xid();
    const auto version = program == vxi11::kPortmapperProgram
                             ? vxi11::kPortmapperVersion
                             : vxi11::kProgramVersion;
    auto request = vxi11::make_rpc_call(xid, program, version,
                                        procedure, arguments);
    if (request.empty()) {
        return VI_ERROR_INV_SIZE;
    }
    std::vector<std::uint8_t> response;
    const auto status = channel.exchange(operation, std::move(request),
                                         ResponseFraming::rpc_record, response);
    if (status < VI_SUCCESS) {
        return status;
    }
    std::span<const std::uint8_t> payload;
    if (!vxi11::parse_rpc_reply(response, xid, payload)) {
        return VI_ERROR_IO;
    }
    result.assign(payload.begin(), payload.end());
    return VI_SUCCESS;
}

ViStatus Vxi11BackendSession::open(ViUInt32 timeout) {
    Operation operation(timeout);
    auto portmapper = std::make_unique<RequestChannel>(64u * 1024u);
    std::string ignored_address;
    auto status = portmapper->connect(host_, portmapper_port_,
                                      remaining_timeout(operation), ignored_address);
    if (status < VI_SUCCESS) {
        return status;
    }

    vxi11::XdrWriter mapping;
    mapping.u32(vxi11::kDeviceCoreProgram);
    mapping.u32(vxi11::kProgramVersion);
    mapping.u32(vxi11::kTcpProtocol);
    mapping.u32(0);
    std::vector<std::uint8_t> result;
    status = call(*portmapper, operation, vxi11::kPortmapperProgram,
                  vxi11::kPortmapperGetPort, mapping.bytes(), result);
    portmapper->close();
    if (status < VI_SUCCESS) {
        return status;
    }
    vxi11::XdrReader port_reader(result);
    std::uint32_t core_port = 0;
    if (!port_reader.u32(core_port) || core_port == 0 || core_port > UINT16_MAX) {
        return VI_ERROR_RSRC_NFOUND;
    }
    core_port_ = static_cast<std::uint16_t>(core_port);
    status = core_->connect(host_, core_port_, remaining_timeout(operation), address_);
    if (status < VI_SUCCESS) {
        return status;
    }

    vxi11::XdrWriter create;
    create.u32(next_xid());
    create.boolean(false);
    create.u32(remaining_timeout(operation));
    create.string(device_);
    status = call(*core_, operation, vxi11::kDeviceCoreProgram,
                  vxi11::kCreateLink, create.bytes(), result);
    if (status < VI_SUCCESS) {
        return status;
    }
    vxi11::XdrReader create_reader(result);
    std::uint32_t error = 0;
    std::uint32_t abort_port = 0;
    std::uint32_t maximum_receive = 0;
    if (!create_reader.u32(error) || !create_reader.u32(link_id_) ||
        !create_reader.u32(abort_port) || abort_port > UINT16_MAX ||
        !create_reader.u32(maximum_receive)) {
        return VI_ERROR_IO;
    }
    abort_port_ = static_cast<std::uint16_t>(abort_port);
    status = map_device_error(error);
    if (status < VI_SUCCESS) {
        return status;
    }
    if (maximum_receive != 0) {
        maximum_receive_size_ =
            std::min<std::uint32_t>(maximum_receive,
                                    static_cast<std::uint32_t>(kMaximumRpcPayload));
    }
    if (abort_port_ == 0) {
        return VI_ERROR_IO;
    }
    status = abort_->connect(host_, abort_port_, remaining_timeout(operation),
                             ignored_address);
    if (status < VI_SUCCESS) {
        return status;
    }
    core_->set_cancel_observer([this] { request_abort(); });
    return VI_SUCCESS;
}

bool Vxi11BackendSession::acquire_io(
    Operation& operation, std::unique_lock<std::timed_mutex>& lock) {
    while (!operation.completed()) {
        if (operation.deadline() != Operation::Clock::time_point::max() &&
            operation.deadline() <= Operation::Clock::now()) {
            static_cast<void>(operation.request_timeout());
            break;
        }
        if (lock.try_lock_for(std::chrono::milliseconds(5))) {
            abort_requested_.store(false, std::memory_order_release);
            return true;
        }
    }
    return false;
}

ViStatus Vxi11BackendSession::write(Operation& operation, ViConstBuf buffer,
                                    ViUInt32 count, ViPUInt32 return_count,
                                    WriteOptions options) {
    *return_count = 0;
    std::unique_lock<std::timed_mutex> lock(io_mutex_, std::defer_lock);
    if (!acquire_io(operation, lock)) {
        return operation.result();
    }
    std::uint32_t offset = 0;
    while (offset < count) {
        const auto amount = std::min<std::uint32_t>(count - offset,
                                                    maximum_receive_size_);
        vxi11::XdrWriter arguments;
        arguments.u32(link_id_);
        arguments.u32(remaining_timeout(operation));
        arguments.u32(remaining_timeout(operation));
        const bool last = offset + amount == count;
        arguments.u32(last && options.send_end ? vxi11::kFlagEnd : 0u);
        arguments.opaque(std::span<const std::uint8_t>(buffer + offset, amount));
        std::vector<std::uint8_t> result;
        auto status = call(*core_, operation, vxi11::kDeviceCoreProgram,
                           vxi11::kDeviceWrite, arguments.bytes(), result);
        if (status < VI_SUCCESS) {
            return status;
        }
        vxi11::XdrReader reader(result);
        std::uint32_t error = 0;
        std::uint32_t written = 0;
        if (!reader.u32(error) || !reader.u32(written) || written > amount) {
            return VI_ERROR_IO;
        }
        status = map_device_error(error);
        if (status < VI_SUCCESS) {
            static_cast<void>(operation.try_complete(status));
            return operation.result();
        }
        offset += written;
        if (written != amount) {
            static_cast<void>(operation.try_complete(VI_ERROR_IO));
            return operation.result();
        }
    }
    if (!operation.try_complete(VI_SUCCESS)) {
        return operation.result();
    }
    *return_count = count;
    return VI_SUCCESS;
}

ViStatus Vxi11BackendSession::read(Operation& operation, ViPBuf buffer,
                                   ViUInt32 count, ViPUInt32 return_count,
                                   ReadOptions options) {
    *return_count = 0;
    std::unique_lock<std::timed_mutex> lock(io_mutex_, std::defer_lock);
    if (!acquire_io(operation, lock)) {
        return operation.result();
    }
    std::vector<ViByte> data;
    data.reserve(std::min<std::size_t>(count, 4096u));
    const auto restore_data = [&] {
        std::lock_guard ahead_lock(read_ahead_mutex_);
        for (auto iterator = data.rbegin(); iterator != data.rend(); ++iterator) {
            read_ahead_.push_front(*iterator);
        }
    };
    {
        std::lock_guard ahead_lock(read_ahead_mutex_);
        while (!read_ahead_.empty() && data.size() < count) {
            const auto byte = read_ahead_.front();
            read_ahead_.pop_front();
            data.push_back(byte);
            if (options.termchar_enabled && byte == options.termchar) {
                break;
            }
        }
    }
    bool ended = options.termchar_enabled && !data.empty() &&
                 data.back() == options.termchar;
    bool ended_by_termchar = ended;
    while (!ended && data.size() < count) {
        const auto request_size = static_cast<std::uint32_t>(
            std::min<std::size_t>(count - data.size(), maximum_receive_size_));
        vxi11::XdrWriter arguments;
        arguments.u32(link_id_);
        arguments.u32(request_size);
        arguments.u32(remaining_timeout(operation));
        arguments.u32(remaining_timeout(operation));
        arguments.u32(options.termchar_enabled ? vxi11::kFlagTermCharSet : 0u);
        arguments.u32(options.termchar);
        std::vector<std::uint8_t> result;
        auto status = call(*core_, operation, vxi11::kDeviceCoreProgram,
                           vxi11::kDeviceRead, arguments.bytes(), result);
        if (status < VI_SUCCESS) {
            restore_data();
            return status;
        }
        vxi11::XdrReader reader(result);
        std::uint32_t error = 0;
        std::uint32_t reason = 0;
        std::vector<std::uint8_t> chunk;
        if (!reader.u32(error) || !reader.u32(reason) ||
            !reader.opaque(chunk, kMaximumRpcPayload)) {
            restore_data();
            return VI_ERROR_IO;
        }
        status = map_device_error(error);
        if (status < VI_SUCCESS) {
            static_cast<void>(operation.try_complete(status));
            restore_data();
            return operation.result();
        }
        if (chunk.empty() && reason == 0) {
            restore_data();
            return VI_ERROR_IO;
        }
        for (std::size_t index = 0; index < chunk.size(); ++index) {
            const auto byte = chunk[index];
            if (data.size() < count) {
                data.push_back(byte);
            } else {
                std::lock_guard ahead_lock(read_ahead_mutex_);
                read_ahead_.push_back(byte);
            }
            if (options.termchar_enabled && byte == options.termchar) {
                std::lock_guard ahead_lock(read_ahead_mutex_);
                for (std::size_t remaining = index + 1u; remaining < chunk.size();
                     ++remaining) {
                    read_ahead_.push_back(chunk[remaining]);
                }
                ended = true;
                ended_by_termchar = true;
                break;
            }
        }
        ended = ended || (reason & vxi11::kReasonEnd) != 0u ||
                (reason & vxi11::kReasonTermChar) != 0u;
        ended_by_termchar = ended_by_termchar ||
                            (reason & vxi11::kReasonTermChar) != 0u;
    }
    const auto status = ended_by_termchar
                            ? VI_SUCCESS_TERM_CHAR
                            : (data.size() == count ? VI_SUCCESS_MAX_CNT : VI_SUCCESS);
    if (!operation.try_complete(status)) {
        restore_data();
        return operation.result();
    }
    std::memcpy(buffer, data.data(), data.size());
    *return_count = static_cast<ViUInt32>(data.size());
    return status;
}

ViStatus Vxi11BackendSession::generic_call(Operation& operation,
                                           std::uint32_t procedure) {
    std::unique_lock<std::timed_mutex> lock(io_mutex_, std::defer_lock);
    if (!acquire_io(operation, lock)) {
        return operation.result();
    }
    vxi11::XdrWriter arguments;
    arguments.u32(link_id_);
    arguments.u32(remaining_timeout(operation) == 0 ? 0u : vxi11::kFlagWaitLock);
    arguments.u32(remaining_timeout(operation));
    arguments.u32(remaining_timeout(operation));
    std::vector<std::uint8_t> result;
    auto status = call(*core_, operation, vxi11::kDeviceCoreProgram, procedure,
                       arguments.bytes(), result);
    if (status < VI_SUCCESS) {
        return status;
    }
    vxi11::XdrReader reader(result);
    std::uint32_t error = 0;
    if (!reader.u32(error)) {
        return VI_ERROR_IO;
    }
    return map_device_error(error);
}

ViStatus Vxi11BackendSession::clear(Operation& operation) {
    const auto status = generic_call(operation, vxi11::kDeviceClear);
    if (status >= VI_SUCCESS) {
        std::lock_guard lock(read_ahead_mutex_);
        read_ahead_.clear();
    }
    return status;
}

ViStatus Vxi11BackendSession::assert_trigger(Operation& operation,
                                             ViUInt16 protocol) {
    if (protocol != VI_TRIG_PROT_DEFAULT) {
        return VI_ERROR_INV_PROT;
    }
    return generic_call(operation, vxi11::kDeviceTrigger);
}

ViStatus Vxi11BackendSession::read_stb(Operation& operation, ViPUInt16 status_byte) {
    if (status_byte == nullptr) {
        return VI_ERROR_INV_PARAMETER;
    }
    std::unique_lock<std::timed_mutex> lock(io_mutex_, std::defer_lock);
    if (!acquire_io(operation, lock)) {
        return operation.result();
    }
    vxi11::XdrWriter arguments;
    arguments.u32(link_id_);
    arguments.u32(vxi11::kFlagWaitLock);
    arguments.u32(remaining_timeout(operation));
    arguments.u32(remaining_timeout(operation));
    std::vector<std::uint8_t> result;
    auto status = call(*core_, operation, vxi11::kDeviceCoreProgram,
                       vxi11::kDeviceReadStb, arguments.bytes(), result);
    if (status < VI_SUCCESS) {
        return status;
    }
    vxi11::XdrReader reader(result);
    std::uint32_t error = 0;
    std::uint32_t value = 0;
    if (!reader.u32(error) || !reader.u32(value) || value > UINT8_MAX) {
        return VI_ERROR_IO;
    }
    status = map_device_error(error);
    if (status >= VI_SUCCESS) {
        *status_byte = static_cast<ViUInt16>(value);
    }
    return status;
}

ViStatus Vxi11BackendSession::lock(Operation& operation, ViAccessMode lock_type,
                                   const std::string& access_key) {
    static_cast<void>(access_key);
    if (lock_type == VI_SHARED_LOCK) {
        return VI_SUCCESS;
    }
    std::unique_lock<std::timed_mutex> lock(io_mutex_, std::defer_lock);
    if (!acquire_io(operation, lock)) {
        return operation.result();
    }
    vxi11::XdrWriter arguments;
    arguments.u32(link_id_);
    arguments.u32(vxi11::kFlagWaitLock);
    arguments.u32(remaining_timeout(operation));
    std::vector<std::uint8_t> result;
    auto status = call(*core_, operation, vxi11::kDeviceCoreProgram,
                       vxi11::kDeviceLock, arguments.bytes(), result);
    if (status < VI_SUCCESS) {
        return status;
    }
    vxi11::XdrReader reader(result);
    std::uint32_t error = 0;
    return reader.u32(error) ? map_device_error(error) : VI_ERROR_IO;
}

ViStatus Vxi11BackendSession::unlock(Operation& operation) {
    std::unique_lock<std::timed_mutex> lock(io_mutex_, std::defer_lock);
    if (!acquire_io(operation, lock)) {
        return operation.result();
    }
    vxi11::XdrWriter arguments;
    arguments.u32(link_id_);
    std::vector<std::uint8_t> result;
    auto status = call(*core_, operation, vxi11::kDeviceCoreProgram,
                       vxi11::kDeviceUnlock, arguments.bytes(), result);
    if (status < VI_SUCCESS) {
        return status;
    }
    vxi11::XdrReader reader(result);
    std::uint32_t error = 0;
    return reader.u32(error) ? map_device_error(error) : VI_ERROR_IO;
}

ViStatus Vxi11BackendSession::flush(ViUInt16 mask) {
    if (!valid_flush_mask(mask)) {
        return VI_ERROR_INV_MASK;
    }
    if ((mask & (VI_READ_BUF_DISCARD | VI_IO_IN_BUF_DISCARD)) != 0) {
        std::lock_guard lock(read_ahead_mutex_);
        read_ahead_.clear();
    }
    return VI_SUCCESS;
}

ViStatus Vxi11BackendSession::set_attribute(ViAttr attribute, ViAttrState value) {
    static_cast<void>(value);
    if (attribute == VI_ATTR_TCPIP_ADDR || attribute == VI_ATTR_TCPIP_HOSTNAME ||
        attribute == VI_ATTR_TCPIP_PORT) {
        return VI_ERROR_ATTR_READONLY;
    }
    return VI_ERROR_NSUP_ATTR;
}

ViStatus Vxi11BackendSession::get_attribute(ViAttr attribute, void* value) {
    switch (attribute) {
        case VI_ATTR_TCPIP_ADDR:
            copy_string(value, address_);
            return VI_SUCCESS;
        case VI_ATTR_TCPIP_HOSTNAME:
            copy_string(value, host_);
            return VI_SUCCESS;
        case VI_ATTR_TCPIP_PORT:
            *static_cast<ViUInt16*>(value) = core_port_;
            return VI_SUCCESS;
        default:
            return VI_ERROR_NSUP_ATTR;
    }
}

void Vxi11BackendSession::request_abort() noexcept {
    if (!abort_ || link_id_ == 0 || closed_.load(std::memory_order_acquire)) {
        return;
    }
    if (abort_requested_.exchange(true, std::memory_order_acq_rel)) {
        return;
    }
    try {
        Operation operation(250);
        vxi11::XdrWriter arguments;
        arguments.u32(link_id_);
        std::vector<std::uint8_t> result;
        static_cast<void>(call(*abort_, operation, vxi11::kDeviceAsyncProgram,
                               vxi11::kDeviceAbort, arguments.bytes(), result));
    } catch (...) {
    }
}

void Vxi11BackendSession::notify_cancel() noexcept {
    // The active core-channel request owns abort recovery. A second
    // session-wide abort can arrive after recovery and corrupt the next RPC.
}

void Vxi11BackendSession::close() noexcept {
    if (closed_.exchange(true, std::memory_order_acq_rel)) {
        return;
    }
    try {
        if (core_ && link_id_ != 0) {
            Operation operation(250);
            vxi11::XdrWriter arguments;
            arguments.u32(link_id_);
            std::vector<std::uint8_t> result;
            static_cast<void>(call(*core_, operation, vxi11::kDeviceCoreProgram,
                                   vxi11::kDestroyLink, arguments.bytes(), result));
        }
    } catch (...) {
    }
    if (abort_) {
        abort_->close();
    }
    if (core_) {
        core_->close();
    }
}

}  // namespace wrvisa
