#include "backends/usb/libusb_provider.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "backends/usb/usb_provider.h"
#include "resource/resource_parser.h"

#ifndef WRVISA_HAS_LIBUSB
#define WRVISA_HAS_LIBUSB 0
#endif

#if WRVISA_HAS_LIBUSB
#include <libusb.h>
#endif

namespace wrvisa::libusb_detail {

std::optional<UsbInterfaceInfo> inspect_interface(
    const InterfaceDescriptor& descriptor) noexcept {
    constexpr std::uint8_t kApplicationClass = 0xfe;
    constexpr std::uint8_t kUsbTmcSubclass = 0x03;
    constexpr std::uint8_t kDirectionIn = 0x80;
    constexpr std::uint8_t kTransferTypeMask = 0x03;
    constexpr std::uint8_t kBulk = 0x02;
    constexpr std::uint8_t kInterrupt = 0x03;

    if (descriptor.interface_class != kApplicationClass ||
        descriptor.interface_subclass != kUsbTmcSubclass ||
        descriptor.interface_protocol > 1) {
        return std::nullopt;
    }

    UsbInterfaceInfo result;
    result.interface_number = descriptor.number;
    result.usb488 = descriptor.interface_protocol == 1;
    result.alternate_setting = descriptor.alternate_setting;
    for (const auto& endpoint : descriptor.endpoints) {
        const auto type = endpoint.attributes & kTransferTypeMask;
        const bool input = (endpoint.address & kDirectionIn) != 0;
        if (type == kBulk && input) {
            if (result.bulk_in_endpoint != 0 ||
                endpoint.maximum_packet_size == 0) {
                return std::nullopt;
            }
            result.bulk_in_endpoint = endpoint.address;
            result.bulk_in_max_packet_size = endpoint.maximum_packet_size;
        } else if (type == kBulk && !input) {
            if (result.bulk_out_endpoint != 0) {
                return std::nullopt;
            }
            result.bulk_out_endpoint = endpoint.address;
        } else if (type == kInterrupt && input) {
            if (result.interrupt_in_endpoint != 0) {
                return std::nullopt;
            }
            result.interrupt_in_endpoint = endpoint.address;
        }
    }
    if (result.bulk_in_endpoint == 0 || result.bulk_out_endpoint == 0) {
        return std::nullopt;
    }
    return result;
}

std::optional<UsbInterfaceInfo> inspect_raw_interface(
    const InterfaceDescriptor& descriptor,
    const UsbRawConfiguration& configuration) noexcept {
    constexpr std::uint8_t kTransferTypeMask = 0x03;
    constexpr std::uint8_t kBulk = 0x02;
    constexpr std::uint8_t kInterrupt = 0x03;
    if (descriptor.alternate_setting != configuration.alternate_setting) {
        return std::nullopt;
    }

    UsbInterfaceInfo result;
    result.interface_number = descriptor.number;
    result.alternate_setting = descriptor.alternate_setting;
    const auto select_endpoint = [&](UsbTransferType type,
                                     std::uint8_t address,
                                     bool input) -> bool {
        if (type == UsbTransferType::none) {
            return address == 0;
        }
        if (type != UsbTransferType::bulk &&
            type != UsbTransferType::interrupt) {
            return false;
        }
        if (((address & 0x80u) != 0) != input ||
            (address & 0x0fu) == 0) {
            return false;
        }
        const auto expected = type == UsbTransferType::bulk ? kBulk : kInterrupt;
        const auto found = std::find_if(
            descriptor.endpoints.begin(), descriptor.endpoints.end(),
            [&](const EndpointDescriptor& endpoint) {
                return endpoint.address == address &&
                       (endpoint.attributes & kTransferTypeMask) == expected &&
                       endpoint.maximum_packet_size != 0;
            });
        if (found == descriptor.endpoints.end()) {
            return false;
        }
        if (type == UsbTransferType::bulk) {
            if (input) {
                result.bulk_in_endpoint = address;
                result.bulk_in_max_packet_size = found->maximum_packet_size;
            } else {
                result.bulk_out_endpoint = address;
            }
        } else if (input) {
            result.interrupt_in_endpoint = address;
        } else {
            result.interrupt_out_endpoint = address;
        }
        return true;
    };
    if (!select_endpoint(configuration.read_transfer_type,
                         configuration.read_endpoint, true) ||
        !select_endpoint(configuration.write_transfer_type,
                         configuration.write_endpoint, false)) {
        return std::nullopt;
    }
    return result;
}

ViStatus map_error(int error, bool opening) noexcept {
    switch (error) {
        case 0:
            return VI_SUCCESS;
        case -2:
            return VI_ERROR_INV_PARAMETER;
        case -3:
            return VI_ERROR_NPERMISSION;
        case -4:
            return opening ? VI_ERROR_RSRC_NFOUND : VI_ERROR_CONN_LOST;
        case -5:
            return opening ? VI_ERROR_RSRC_NFOUND : VI_ERROR_IO;
        case -6:
            return VI_ERROR_RSRC_BUSY;
        case -7:
            return VI_ERROR_TMO;
        case -11:
            return VI_ERROR_ALLOC;
        case -12:
            return VI_ERROR_NSUP_OPER;
        default:
            return VI_ERROR_IO;
    }
}

bool provider_available() noexcept { return WRVISA_HAS_LIBUSB != 0; }

}  // namespace wrvisa::libusb_detail

namespace wrvisa {

#if WRVISA_HAS_LIBUSB
namespace {

class LibusbProvider;

class LibusbRuntime final {
public:
    static std::shared_ptr<LibusbRuntime> create(ViStatus& status) {
        libusb_context* context = nullptr;
        const auto result = libusb_init(&context);
        if (result < 0) {
            status = libusb_detail::map_error(result, true);
            return nullptr;
        }
        status = VI_SUCCESS;
        return std::shared_ptr<LibusbRuntime>(new LibusbRuntime(context));
    }

    ~LibusbRuntime() {
        stop_events();
        libusb_exit(context_);
    }

    libusb_context* context() const noexcept { return context_; }

    bool register_hotplug(libusb_hotplug_callback_fn callback,
                          void* user_data,
                          libusb_hotplug_callback_handle& handle) {
        if (libusb_has_capability(LIBUSB_CAP_HAS_HOTPLUG) == 0) {
            return false;
        }
        return libusb_hotplug_register_callback(
                   context_, LIBUSB_HOTPLUG_EVENT_DEVICE_ARRIVED |
                                 LIBUSB_HOTPLUG_EVENT_DEVICE_LEFT,
                   LIBUSB_HOTPLUG_NO_FLAGS, LIBUSB_HOTPLUG_MATCH_ANY,
                   LIBUSB_HOTPLUG_MATCH_ANY, LIBUSB_HOTPLUG_MATCH_ANY,
                   callback, user_data, &handle) == LIBUSB_SUCCESS;
    }

    void deregister_hotplug(libusb_hotplug_callback_handle handle) noexcept {
        libusb_hotplug_deregister_callback(context_, handle);
    }

    void retain_handle() {
        std::lock_guard lock(events_mutex_);
        ++open_handles_;
        if (open_handles_ != 1) {
            return;
        }
        events_running_.store(true, std::memory_order_release);
        events_thread_ = std::thread([this] { event_loop(); });
    }

    void close_handle(libusb_device_handle* handle) noexcept {
        std::thread joiner;
        {
            std::lock_guard lock(events_mutex_);
            if (open_handles_ == 0) {
                libusb_close(handle);
                return;
            }
            --open_handles_;
            const bool last = open_handles_ == 0;
            if (last) {
                events_running_.store(false, std::memory_order_release);
            }
            libusb_close(handle);
            if (last) {
                joiner = std::move(events_thread_);
            }
        }
        if (joiner.joinable()) {
            joiner.join();
        }
    }

private:
    explicit LibusbRuntime(libusb_context* context) : context_(context) {}

    void event_loop() noexcept {
        while (events_running_.load(std::memory_order_acquire)) {
            timeval timeout{};
            timeout.tv_usec = 50'000;
            const auto result =
                libusb_handle_events_timeout_completed(context_, &timeout,
                                                       nullptr);
            if (result < 0 && result != LIBUSB_ERROR_INTERRUPTED) {
                std::this_thread::yield();
            }
        }
    }

    void stop_events() noexcept {
        std::thread joiner;
        {
            std::lock_guard lock(events_mutex_);
            events_running_.store(false, std::memory_order_release);
            joiner = std::move(events_thread_);
        }
        if (joiner.joinable()) {
            joiner.join();
        }
    }

    libusb_context* context_{nullptr};
    std::mutex events_mutex_;
    std::atomic<bool> events_running_{false};
    std::size_t open_handles_{0};
    std::thread events_thread_;
};

class LibusbHandle final {
public:
    LibusbHandle(std::shared_ptr<LibusbRuntime> runtime,
                 libusb_device_handle* handle, std::uint8_t interface_number,
                 std::uint8_t alternate_setting)
        : runtime_(std::move(runtime)),
          handle_(handle),
          interface_number_(interface_number),
          alternate_setting_(alternate_setting) {
        runtime_->retain_handle();
    }

    ~LibusbHandle() {
        release();
        if (handle_ != nullptr) {
            runtime_->close_handle(std::exchange(handle_, nullptr));
        }
    }

    libusb_device_handle* get() const noexcept { return handle_; }

    ViStatus claim() noexcept {
        const auto detach = libusb_set_auto_detach_kernel_driver(handle_, 1);
        if (detach < 0 && detach != LIBUSB_ERROR_NOT_SUPPORTED) {
            return libusb_detail::map_error(detach, true);
        }
        auto result = libusb_claim_interface(handle_, interface_number_);
        if (result < 0) {
            return libusb_detail::map_error(result, true);
        }
        claimed_ = true;
        if (alternate_setting_ != 0) {
            result = libusb_set_interface_alt_setting(
                handle_, interface_number_, alternate_setting_);
            if (result < 0) {
                release();
                return libusb_detail::map_error(result, true);
            }
        }
        return VI_SUCCESS;
    }

    void release() noexcept {
        if (claimed_) {
            static_cast<void>(
                libusb_release_interface(handle_, interface_number_));
            claimed_ = false;
        }
    }

private:
    std::shared_ptr<LibusbRuntime> runtime_;
    libusb_device_handle* handle_{nullptr};
    std::uint8_t interface_number_{0};
    std::uint8_t alternate_setting_{0};
    bool claimed_{false};
};

class EndpointGate final {
public:
    class Lease final {
    public:
        Lease() = default;
        explicit Lease(EndpointGate* gate) : gate_(gate) {}
        Lease(const Lease&) = delete;
        Lease& operator=(const Lease&) = delete;
        Lease(Lease&& other) noexcept
            : gate_(std::exchange(other.gate_, nullptr)) {}
        Lease& operator=(Lease&& other) noexcept {
            if (this != &other) {
                reset();
                gate_ = std::exchange(other.gate_, nullptr);
            }
            return *this;
        }
        ~Lease() { reset(); }

    private:
        void reset() noexcept {
            if (gate_ != nullptr) {
                gate_->release();
                gate_ = nullptr;
            }
        }
        EndpointGate* gate_{nullptr};
    };

    std::optional<Lease> acquire(Operation& operation, ViStatus& status) {
        std::unique_lock lock(mutex_);
        while (busy_) {
            if (operation.completed()) {
                status = operation.result();
                return std::nullopt;
            }
            if (operation.deadline() == Operation::Clock::time_point::max()) {
                condition_.wait_for(lock, std::chrono::milliseconds(50));
            } else if (condition_.wait_until(lock, operation.deadline()) ==
                       std::cv_status::timeout) {
                static_cast<void>(operation.request_timeout());
                status = operation.result();
                return std::nullopt;
            }
        }
        if (operation.completed()) {
            status = operation.result();
            return std::nullopt;
        }
        busy_ = true;
        status = VI_SUCCESS;
        return Lease(this);
    }

    void notify() noexcept { condition_.notify_all(); }

private:
    void release() noexcept {
        {
            std::lock_guard lock(mutex_);
            busy_ = false;
        }
        condition_.notify_one();
    }

    std::mutex mutex_;
    std::condition_variable condition_;
    bool busy_{false};
};

class LibusbConnection final {
public:
    LibusbConnection(std::shared_ptr<LibusbProvider> provider,
                     libusb_device* device,
                     std::shared_ptr<LibusbHandle> handle,
                     std::shared_ptr<UsbInterfaceLease> lease,
                     std::uint8_t alternate_setting, std::string identity)
        : provider_(std::move(provider)),
          device_(libusb_ref_device(device)),
          handle_(std::move(handle)),
          lease_(std::move(lease)),
          alternate_setting_(alternate_setting),
          identity_(std::move(identity)) {}

    ~LibusbConnection() {
        mark_disconnected();
        lease_.reset();
        handle_.reset();
        libusb_unref_device(device_);
    }

    libusb_device* device() const noexcept { return device_; }
    libusb_device_handle* handle() const noexcept { return handle_->get(); }
    std::uint8_t alternate_setting() const noexcept {
        return alternate_setting_;
    }
    const std::string& identity() const noexcept { return identity_; }
    bool connected() const noexcept {
        return connected_.load(std::memory_order_acquire);
    }

    void mark_disconnected() noexcept {
        connected_.store(false, std::memory_order_release);
        bulk_out_gate_.notify();
        bulk_in_gate_.notify();
        interrupt_gate_.notify();
        control_gate_.notify();
    }

    void notify_gates() noexcept {
        bulk_out_gate_.notify();
        bulk_in_gate_.notify();
        interrupt_gate_.notify();
        control_gate_.notify();
    }

    EndpointGate& bulk_out_gate() noexcept { return bulk_out_gate_; }
    EndpointGate& bulk_in_gate() noexcept { return bulk_in_gate_; }
    EndpointGate& interrupt_gate() noexcept { return interrupt_gate_; }
    EndpointGate& control_gate() noexcept { return control_gate_; }

private:
    std::shared_ptr<LibusbProvider> provider_;
    libusb_device* device_{nullptr};
    std::shared_ptr<LibusbHandle> handle_;
    std::shared_ptr<UsbInterfaceLease> lease_;
    std::uint8_t alternate_setting_{0};
    std::string identity_;
    std::atomic<bool> connected_{true};
    EndpointGate bulk_out_gate_;
    EndpointGate bulk_in_gate_;
    EndpointGate interrupt_gate_;
    EndpointGate control_gate_;
};

struct TransferWait {
    std::mutex mutex;
    std::condition_variable condition;
    bool completed{false};
    libusb_transfer_status status{LIBUSB_TRANSFER_ERROR};
    int actual_length{0};
};

void LIBUSB_CALL transfer_complete(libusb_transfer* transfer) {
    auto* wait = static_cast<TransferWait*>(transfer->user_data);
    {
        std::lock_guard lock(wait->mutex);
        wait->status = transfer->status;
        wait->actual_length = transfer->actual_length;
        wait->completed = true;
    }
    wait->condition.notify_one();
}

std::optional<unsigned int> transfer_timeout(Operation& operation) {
    if (operation.completed()) {
        return std::nullopt;
    }
    if (operation.deadline() == Operation::Clock::time_point::max()) {
        return 0;
    }
    const auto now = Operation::Clock::now();
    if (now >= operation.deadline()) {
        static_cast<void>(operation.request_timeout());
        return std::nullopt;
    }
    const auto remaining = operation.deadline() - now;
    const auto milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(
        remaining + std::chrono::milliseconds(1) -
        Operation::Clock::duration(1));
    return static_cast<unsigned int>(std::min<std::uint64_t>(
        static_cast<std::uint64_t>(milliseconds.count()),
        std::numeric_limits<unsigned int>::max()));
}

class LibusbTransport final : public UsbTransport {
public:
    LibusbTransport(std::shared_ptr<LibusbConnection> connection,
                    UsbInterfaceInfo info)
        : connection_(std::move(connection)), info_(info) {}
    ~LibusbTransport() override { close(); }

    UsbInterfaceInfo interface_info() const noexcept override {
        return info_;
    }

    ViStatus bulk_out(Operation& operation,
                      std::span<const std::uint8_t> data,
                      std::size_t& transferred) override {
        transferred = 0;
        if (data.size() > static_cast<std::size_t>(
                              std::numeric_limits<int>::max())) {
            return VI_ERROR_INV_SIZE;
        }
        ViStatus gate_status = VI_SUCCESS;
        auto gate = connection_->bulk_out_gate().acquire(operation, gate_status);
        if (!gate) {
            return gate_status;
        }
        const auto timeout = transfer_timeout(operation);
        if (!timeout) {
            return operation.result();
        }
        std::vector<unsigned char> buffer(data.begin(), data.end());
        auto* transfer = libusb_alloc_transfer(0);
        if (transfer == nullptr) {
            return VI_ERROR_ALLOC;
        }
        TransferWait wait;
        libusb_fill_bulk_transfer(
            transfer, connection_->handle(),
            info_.bulk_out_endpoint, buffer.data(),
            static_cast<int>(buffer.size()), transfer_complete, &wait,
            *timeout);
        const auto status = submit_and_wait(operation, transfer, wait,
                                            transferred);
        libusb_free_transfer(transfer);
        return status;
    }

    ViStatus bulk_in(Operation& operation, std::size_t maximum_size,
                     std::vector<std::uint8_t>& data) override {
        data.clear();
        if (maximum_size > static_cast<std::size_t>(
                               std::numeric_limits<int>::max())) {
            return VI_ERROR_INV_SIZE;
        }
        ViStatus gate_status = VI_SUCCESS;
        auto gate = connection_->bulk_in_gate().acquire(operation, gate_status);
        if (!gate) {
            return gate_status;
        }
        const auto timeout = transfer_timeout(operation);
        if (!timeout) {
            return operation.result();
        }
        data.resize(maximum_size);
        auto* transfer = libusb_alloc_transfer(0);
        if (transfer == nullptr) {
            data.clear();
            return VI_ERROR_ALLOC;
        }
        TransferWait wait;
        libusb_fill_bulk_transfer(
            transfer, connection_->handle(),
            info_.bulk_in_endpoint, data.data(),
            static_cast<int>(data.size()), transfer_complete, &wait, *timeout);
        std::size_t transferred = 0;
        const auto status =
            submit_and_wait(operation, transfer, wait, transferred);
        libusb_free_transfer(transfer);
        if (status < VI_SUCCESS) {
            data.clear();
        } else {
            data.resize(transferred);
        }
        return status;
    }

    ViStatus control_in(Operation& operation,
                        const UsbControlRequest& request,
                        std::size_t response_size,
                        std::vector<std::uint8_t>& response) override {
        response.clear();
        if (response_size > std::numeric_limits<std::uint16_t>::max()) {
            return VI_ERROR_INV_SIZE;
        }
        ViStatus gate_status = VI_SUCCESS;
        auto gate = connection_->control_gate().acquire(operation, gate_status);
        if (!gate) {
            return gate_status;
        }
        const auto timeout = transfer_timeout(operation);
        if (!timeout) {
            return operation.result();
        }
        std::vector<unsigned char> buffer(LIBUSB_CONTROL_SETUP_SIZE +
                                          response_size);
        libusb_fill_control_setup(
            buffer.data(), request.request_type, request.request,
            request.value, request.index,
            static_cast<std::uint16_t>(response_size));
        auto* transfer = libusb_alloc_transfer(0);
        if (transfer == nullptr) {
            return VI_ERROR_ALLOC;
        }
        TransferWait wait;
        libusb_fill_control_transfer(transfer, connection_->handle(),
                                     buffer.data(), transfer_complete, &wait,
                                     *timeout);
        std::size_t transferred = 0;
        const auto status =
            submit_and_wait(operation, transfer, wait, transferred);
        libusb_free_transfer(transfer);
        if (status >= VI_SUCCESS) {
            const auto* payload = buffer.data() + LIBUSB_CONTROL_SETUP_SIZE;
            response.assign(payload, payload + transferred);
        }
        return status;
    }

    ViStatus control_out(Operation& operation,
                         const UsbControlRequest& request,
                         std::span<const std::uint8_t> data,
                         std::size_t& transferred) override {
        transferred = 0;
        if (data.size() > std::numeric_limits<std::uint16_t>::max()) {
            return VI_ERROR_INV_SIZE;
        }
        ViStatus gate_status = VI_SUCCESS;
        auto gate = connection_->control_gate().acquire(operation, gate_status);
        if (!gate) {
            return gate_status;
        }
        const auto timeout = transfer_timeout(operation);
        if (!timeout) {
            return operation.result();
        }
        std::vector<unsigned char> buffer(LIBUSB_CONTROL_SETUP_SIZE +
                                          data.size());
        libusb_fill_control_setup(
            buffer.data(), request.request_type, request.request,
            request.value, request.index,
            static_cast<std::uint16_t>(data.size()));
        std::copy(data.begin(), data.end(),
                  buffer.begin() + LIBUSB_CONTROL_SETUP_SIZE);
        auto* transfer = libusb_alloc_transfer(0);
        if (transfer == nullptr) {
            return VI_ERROR_ALLOC;
        }
        TransferWait wait;
        libusb_fill_control_transfer(transfer, connection_->handle(),
                                     buffer.data(), transfer_complete, &wait,
                                     *timeout);
        const auto status =
            submit_and_wait(operation, transfer, wait, transferred);
        libusb_free_transfer(transfer);
        return status;
    }

    ViStatus interrupt_in(Operation& operation, std::size_t maximum_size,
                          std::vector<std::uint8_t>& data) override {
        data.clear();
        if (info_.interrupt_in_endpoint == 0) {
            return VI_ERROR_NSUP_OPER;
        }
        if (maximum_size > static_cast<std::size_t>(
                               std::numeric_limits<int>::max())) {
            return VI_ERROR_INV_SIZE;
        }
        ViStatus gate_status = VI_SUCCESS;
        auto gate =
            connection_->interrupt_gate().acquire(operation, gate_status);
        if (!gate) {
            return gate_status;
        }
        const auto timeout = transfer_timeout(operation);
        if (!timeout) {
            return operation.result();
        }
        data.resize(maximum_size);
        auto* transfer = libusb_alloc_transfer(0);
        if (transfer == nullptr) {
            data.clear();
            return VI_ERROR_ALLOC;
        }
        TransferWait wait;
        libusb_fill_interrupt_transfer(
            transfer, connection_->handle(),
            info_.interrupt_in_endpoint, data.data(),
            static_cast<int>(data.size()), transfer_complete, &wait, *timeout);
        std::size_t transferred = 0;
        const auto status =
            submit_and_wait(operation, transfer, wait, transferred);
        libusb_free_transfer(transfer);
        if (status < VI_SUCCESS) {
            data.clear();
        } else {
            data.resize(transferred);
        }
        return status;
    }

    ViStatus interrupt_out(Operation& operation,
                           std::span<const std::uint8_t> data,
                           std::size_t& transferred) override {
        transferred = 0;
        if (info_.interrupt_out_endpoint == 0) {
            return VI_ERROR_NSUP_OPER;
        }
        if (data.size() > static_cast<std::size_t>(
                              std::numeric_limits<int>::max())) {
            return VI_ERROR_INV_SIZE;
        }
        ViStatus gate_status = VI_SUCCESS;
        auto gate =
            connection_->interrupt_gate().acquire(operation, gate_status);
        if (!gate) {
            return gate_status;
        }
        const auto timeout = transfer_timeout(operation);
        if (!timeout) {
            return operation.result();
        }
        std::vector<unsigned char> buffer(data.begin(), data.end());
        auto* transfer = libusb_alloc_transfer(0);
        if (transfer == nullptr) {
            return VI_ERROR_ALLOC;
        }
        TransferWait wait;
        libusb_fill_interrupt_transfer(
            transfer, connection_->handle(), info_.interrupt_out_endpoint,
            buffer.data(), static_cast<int>(buffer.size()), transfer_complete,
            &wait, *timeout);
        const auto status =
            submit_and_wait(operation, transfer, wait, transferred);
        libusb_free_transfer(transfer);
        return status;
    }

    ViStatus clear_halt(Operation& operation,
                        std::uint8_t endpoint_address) override {
        if (endpoint_address == 0) {
            return VI_ERROR_INV_PARAMETER;
        }
        EndpointGate* endpoint_gate = nullptr;
        if (endpoint_address == info_.bulk_out_endpoint) {
            endpoint_gate = &connection_->bulk_out_gate();
        } else if (endpoint_address == info_.bulk_in_endpoint) {
            endpoint_gate = &connection_->bulk_in_gate();
        } else if (endpoint_address == info_.interrupt_out_endpoint) {
            endpoint_gate = &connection_->interrupt_gate();
        } else if (endpoint_address == info_.interrupt_in_endpoint) {
            endpoint_gate = &connection_->interrupt_gate();
        } else {
            return VI_ERROR_INV_PARAMETER;
        }
        ViStatus gate_status = VI_SUCCESS;
        auto gate = endpoint_gate->acquire(operation, gate_status);
        if (!gate) {
            return gate_status;
        }
        if (closed() || !connection_->connected()) {
            return VI_ERROR_CONN_LOST;
        }
        const auto result =
            libusb_clear_halt(connection_->handle(), endpoint_address);
        if (result == LIBUSB_ERROR_NO_DEVICE) {
            connection_->mark_disconnected();
        }
        return libusb_detail::map_error(result, false);
    }

    void cancel() noexcept override {
        {
            // Keep every pointer protected through libusb_cancel_transfer().
            // Waiters take the same mutex before removing and then freeing a
            // transfer, avoiding cancel/free races across concurrent endpoints.
            std::lock_guard lock(state_mutex_);
            for (auto* transfer : active_transfers_) {
                static_cast<void>(libusb_cancel_transfer(transfer));
            }
        }
        connection_->notify_gates();
    }

    void close() noexcept override {
        {
            std::lock_guard lock(state_mutex_);
            closed_ = true;
        }
        cancel();
    }

private:
    bool closed() const noexcept {
        std::lock_guard lock(state_mutex_);
        return closed_;
    }

    ViStatus submit_and_wait(Operation& operation, libusb_transfer* transfer,
                             TransferWait& wait,
                             std::size_t& transferred) noexcept {
        transferred = 0;
        int submit_status = LIBUSB_SUCCESS;
        {
            std::lock_guard lock(state_mutex_);
            if (closed_ || !connection_->connected()) {
                return VI_ERROR_CONN_LOST;
            }
            if (operation.completed()) {
                return operation.result();
            }
            submit_status = libusb_submit_transfer(transfer);
            if (submit_status >= 0) {
                active_transfers_.insert(transfer);
            }
        }
        if (submit_status < 0) {
            if (submit_status == LIBUSB_ERROR_NO_DEVICE) {
                connection_->mark_disconnected();
            }
            return libusb_detail::map_error(submit_status, false);
        }

        {
            std::unique_lock lock(wait.mutex);
            wait.condition.wait(lock, [&wait] { return wait.completed; });
        }
        {
            std::lock_guard lock(state_mutex_);
            active_transfers_.erase(transfer);
        }
        if (operation.completed()) {
            return operation.result();
        }
        if (wait.actual_length > 0) {
            transferred = static_cast<std::size_t>(wait.actual_length);
        }
        switch (wait.status) {
            case LIBUSB_TRANSFER_COMPLETED:
                return VI_SUCCESS;
            case LIBUSB_TRANSFER_TIMED_OUT:
                static_cast<void>(operation.request_timeout());
                return operation.result();
            case LIBUSB_TRANSFER_CANCELLED:
                return closed() ? VI_ERROR_CONN_LOST : VI_ERROR_ABORT;
            case LIBUSB_TRANSFER_NO_DEVICE:
                connection_->mark_disconnected();
                return VI_ERROR_CONN_LOST;
            case LIBUSB_TRANSFER_ERROR:
            case LIBUSB_TRANSFER_STALL:
            case LIBUSB_TRANSFER_OVERFLOW:
                return VI_ERROR_IO;
        }
        return VI_ERROR_IO;
    }

    std::shared_ptr<LibusbConnection> connection_;
    UsbInterfaceInfo info_;
    mutable std::mutex state_mutex_;
    std::set<libusb_transfer*> active_transfers_;
    bool closed_{false};
};

struct DeviceList final {
    explicit DeviceList(libusb_context* context) {
        count = libusb_get_device_list(context, &devices);
    }
    ~DeviceList() {
        if (devices != nullptr) {
            libusb_free_device_list(devices, 1);
        }
    }
    libusb_device** devices{nullptr};
    ssize_t count{0};
};

struct ConfigDescriptor final {
    ~ConfigDescriptor() {
        if (value != nullptr) {
            libusb_free_config_descriptor(value);
        }
    }
    libusb_config_descriptor* value{nullptr};
};

libusb_detail::InterfaceDescriptor convert_interface(
    const libusb_interface_descriptor& descriptor) {
    libusb_detail::InterfaceDescriptor result;
    result.number = descriptor.bInterfaceNumber;
    result.alternate_setting = descriptor.bAlternateSetting;
    result.interface_class = descriptor.bInterfaceClass;
    result.interface_subclass = descriptor.bInterfaceSubClass;
    result.interface_protocol = descriptor.bInterfaceProtocol;
    result.endpoints.reserve(descriptor.bNumEndpoints);
    for (std::uint8_t index = 0; index < descriptor.bNumEndpoints; ++index) {
        const auto& endpoint = descriptor.endpoint[index];
        result.endpoints.push_back({
            endpoint.bEndpointAddress, endpoint.bmAttributes,
            static_cast<std::uint16_t>(endpoint.wMaxPacketSize & 0x07ffu)});
    }
    return result;
}

std::vector<UsbInterfaceInfo> inspect_device(libusb_device* device) {
    ConfigDescriptor config;
    if (libusb_get_active_config_descriptor(device, &config.value) < 0) {
        return {};
    }
    std::vector<UsbInterfaceInfo> result;
    for (std::uint8_t index = 0; index < config.value->bNumInterfaces;
         ++index) {
        const auto& interface = config.value->interface[index];
        for (int alternate = 0; alternate < interface.num_altsetting;
             ++alternate) {
            const auto inspected = libusb_detail::inspect_interface(
                convert_interface(interface.altsetting[alternate]));
            if (inspected) {
                result.push_back(*inspected);
            }
        }
    }
    return result;
}

std::vector<std::uint16_t> raw_interface_numbers(libusb_device* device) {
    ConfigDescriptor config;
    if (libusb_get_active_config_descriptor(device, &config.value) < 0) {
        return {};
    }
    std::vector<std::uint16_t> result;
    for (std::uint8_t index = 0; index < config.value->bNumInterfaces;
         ++index) {
        const auto& interface = config.value->interface[index];
        for (int alternate = 0; alternate < interface.num_altsetting;
             ++alternate) {
            result.push_back(
                interface.altsetting[alternate].bInterfaceNumber);
        }
    }
    std::sort(result.begin(), result.end());
    result.erase(std::unique(result.begin(), result.end()), result.end());
    return result;
}

std::optional<UsbInterfaceInfo> inspect_raw_device(
    libusb_device* device, std::uint16_t interface_number,
    const UsbRawConfiguration& configuration) {
    ConfigDescriptor config;
    if (libusb_get_active_config_descriptor(device, &config.value) < 0) {
        return std::nullopt;
    }
    for (std::uint8_t index = 0; index < config.value->bNumInterfaces;
         ++index) {
        const auto& interface = config.value->interface[index];
        for (int alternate = 0; alternate < interface.num_altsetting;
             ++alternate) {
            const auto& descriptor = interface.altsetting[alternate];
            if (descriptor.bInterfaceNumber != interface_number) {
                continue;
            }
            const auto inspected = libusb_detail::inspect_raw_interface(
                convert_interface(descriptor), configuration);
            if (inspected) {
                return inspected;
            }
        }
    }
    return std::nullopt;
}

std::optional<std::string> read_serial(
    libusb_device* device, const libusb_device_descriptor& descriptor,
    libusb_device_handle*& opened, ViStatus& status) {
    if (descriptor.iSerialNumber == 0) {
        status = VI_ERROR_RSRC_NFOUND;
        return std::nullopt;
    }
    std::array<char, LIBUSB_DEVICE_STRING_BYTES_MAX> buffer{};
    auto result = libusb_get_device_string(
        device, LIBUSB_DEVICE_STRING_SERIAL_NUMBER, buffer.data(),
        static_cast<int>(buffer.size()));
    if (result > 1 && buffer[0] != '\0') {
        status = VI_SUCCESS;
        return std::string(buffer.data());
    }
    if (opened == nullptr) {
        result = libusb_open(device, &opened);
        if (result < 0) {
            status = libusb_detail::map_error(result, true);
            return std::nullopt;
        }
    }
    std::array<unsigned char, 256> ascii{};
    result = libusb_get_string_descriptor_ascii(
        opened, descriptor.iSerialNumber, ascii.data(),
        static_cast<int>(ascii.size()));
    if (result <= 0) {
        status = result < 0 ? libusb_detail::map_error(result, true)
                            : VI_ERROR_RSRC_NFOUND;
        return std::nullopt;
    }
    status = VI_SUCCESS;
    return std::string(reinterpret_cast<const char*>(ascii.data()),
                       static_cast<std::size_t>(result));
}

std::optional<std::string> resource_name(
    std::uint16_t vendor, std::uint16_t product, const std::string& serial,
    std::uint16_t interface_number, std::string_view resource_class) {
    std::ostringstream name;
    name << "USB0::0x" << std::uppercase << std::hex << std::setw(4)
         << std::setfill('0') << vendor << "::0x" << std::setw(4) << product
         << std::dec << "::" << serial << "::" << interface_number
         << "::" << resource_class;
    const auto parsed = parse_resource(name.str());
    if (!parsed || (parsed->kind != ResourceKind::usb_instr &&
                    parsed->kind != ResourceKind::usb_raw)) {
        return std::nullopt;
    }
    return parsed->canonical_name;
}

std::string connection_identity(const ResourceDescriptor& resource,
                                libusb_device* device) {
    std::ostringstream identity;
    identity << "USB#" << resource.usb_vendor_id << ':'
             << resource.usb_product_id << ':' << resource.usb_serial_number
             << ':' << resource.usb_interface_number << '#'
             << static_cast<unsigned>(libusb_get_bus_number(device));
    std::array<std::uint8_t, 8> ports{};
    const auto port_count = libusb_get_port_numbers(
        device, ports.data(), static_cast<int>(ports.size()));
    if (port_count > 0) {
        for (int index = 0; index < port_count; ++index) {
            identity << '.' << static_cast<unsigned>(ports[static_cast<std::size_t>(index)]);
        }
    }
    identity << ':' << static_cast<unsigned>(libusb_get_device_address(device))
             << ':' << static_cast<const void*>(device);
    return identity.str();
}

class LibusbProvider final : public UsbProvider,
                             public std::enable_shared_from_this<LibusbProvider> {
public:
    static std::shared_ptr<LibusbProvider> create() {
        ViStatus status = VI_SUCCESS;
        auto runtime = LibusbRuntime::create(status);
        auto provider = std::shared_ptr<LibusbProvider>(
            new LibusbProvider(std::move(runtime), status));
        provider->register_hotplug();
        return provider;
    }

    ~LibusbProvider() override {
        if (runtime_ && hotplug_registered_) {
            runtime_->deregister_hotplug(hotplug_handle_);
        }
        arbiter_.invalidate_all();
    }

    std::vector<std::string> discover() override {
        std::vector<std::string> resources;
        if (!runtime_) {
            return resources;
        }
        DeviceList devices(runtime_->context());
        if (devices.count < 0) {
            return resources;
        }
        for (ssize_t index = 0; index < devices.count; ++index) {
            auto* device = devices.devices[index];
            libusb_device_descriptor descriptor{};
            if (libusb_get_device_descriptor(device, &descriptor) < 0) {
                continue;
            }
            const auto interfaces = inspect_device(device);
            const auto raw_interfaces = raw_interface_numbers(device);
            if (interfaces.empty() && raw_interfaces.empty()) {
                continue;
            }
            libusb_device_handle* opened = nullptr;
            ViStatus status = VI_SUCCESS;
            const auto serial = read_serial(device, descriptor, opened, status);
            if (opened != nullptr) {
                libusb_close(opened);
            }
            if (!serial) {
                continue;
            }
            for (const auto& interface : interfaces) {
                if (const auto name = resource_name(
                        descriptor.idVendor, descriptor.idProduct, *serial,
                        interface.interface_number, "INSTR")) {
                    resources.push_back(*name);
                }
            }
            for (const auto interface_number : raw_interfaces) {
                if (const auto name = resource_name(
                        descriptor.idVendor, descriptor.idProduct, *serial,
                        interface_number, "RAW")) {
                    resources.push_back(*name);
                }
            }
        }
        return resources;
    }

    std::unique_ptr<UsbTransport> open(
        const ResourceDescriptor& resource, ViUInt32 timeout,
        ViStatus& status) override {
        if (resource.kind != ResourceKind::usb_instr) {
            status = VI_ERROR_RSRC_NFOUND;
            return nullptr;
        }
        return open_impl(resource, nullptr, timeout, status);
    }

    std::unique_ptr<UsbTransport> open_raw(
        const ResourceDescriptor& resource,
        const UsbRawConfiguration& configuration, ViUInt32 timeout,
        ViStatus& status) override {
        if (resource.kind != ResourceKind::usb_raw) {
            status = VI_ERROR_RSRC_NFOUND;
            return nullptr;
        }
        return open_impl(resource, &configuration, timeout, status);
    }

private:
    std::unique_ptr<UsbTransport> open_impl(
        const ResourceDescriptor& resource,
        const UsbRawConfiguration* raw_configuration, ViUInt32 timeout,
        ViStatus& status) {
        static_cast<void>(timeout);
        if (resource.interface_number != 0) {
            status = VI_ERROR_RSRC_NFOUND;
            return nullptr;
        }
        if (!runtime_) {
            status = initialization_status_;
            return nullptr;
        }
        DeviceList devices(runtime_->context());
        if (devices.count < 0) {
            status = libusb_detail::map_error(
                static_cast<int>(devices.count), true);
            return nullptr;
        }

        ViStatus deferred_status = VI_ERROR_RSRC_NFOUND;
        for (ssize_t index = 0; index < devices.count; ++index) {
            auto* device = devices.devices[index];
            libusb_device_descriptor descriptor{};
            if (libusb_get_device_descriptor(device, &descriptor) < 0 ||
                descriptor.idVendor != resource.usb_vendor_id ||
                descriptor.idProduct != resource.usb_product_id) {
                continue;
            }
            std::optional<UsbInterfaceInfo> selected;
            if (raw_configuration != nullptr) {
                selected = inspect_raw_device(
                    device, resource.usb_interface_number,
                    *raw_configuration);
            } else {
                const auto interfaces = inspect_device(device);
                const auto found = std::find_if(
                    interfaces.begin(), interfaces.end(),
                    [&](const auto& value) {
                        return value.interface_number ==
                               resource.usb_interface_number;
                    });
                if (found != interfaces.end()) {
                    selected = *found;
                }
            }
            if (!selected) {
                continue;
            }

            libusb_device_handle* opened = nullptr;
            ViStatus serial_status = VI_SUCCESS;
            const auto serial = read_serial(device, descriptor, opened,
                                            serial_status);
            if (!serial || *serial != resource.usb_serial_number) {
                if (opened != nullptr) {
                    libusb_close(opened);
                }
                if (serial_status == VI_ERROR_NPERMISSION) {
                    deferred_status = serial_status;
                }
                continue;
            }
            const auto identity = connection_identity(resource, device);
            std::lock_guard open_lock(open_mutex_);
            {
                std::lock_guard lock(mutex_);
                if (const auto existing = connections_.find(identity);
                    existing != connections_.end()) {
                    if (auto connection = existing->second.lock();
                        connection && connection->connected()) {
                        if (opened != nullptr) {
                            libusb_close(opened);
                        }
                        if (connection->alternate_setting() !=
                            selected->alternate_setting) {
                            status = VI_ERROR_RSRC_BUSY;
                            return nullptr;
                        }
                        status = VI_SUCCESS;
                        return std::make_unique<LibusbTransport>(
                            std::move(connection), *selected);
                    }
                    connections_.erase(existing);
                }
            }
            if (opened == nullptr) {
                const auto result = libusb_open(device, &opened);
                if (result < 0) {
                    status = libusb_detail::map_error(result, true);
                    return nullptr;
                }
            }

            auto handle = std::make_shared<LibusbHandle>(
                runtime_, opened,
                static_cast<std::uint8_t>(selected->interface_number),
                selected->alternate_setting);
            auto lease = arbiter_.acquire(
                identity, [handle] { return handle->claim(); },
                [handle] { handle->release(); }, status);
            if (!lease) {
                return nullptr;
            }
            auto connection = std::make_shared<LibusbConnection>(
                shared_from_this(), device, std::move(handle),
                std::move(lease), selected->alternate_setting, identity);
            {
                std::lock_guard lock(mutex_);
                if (departed_devices_.contains(device)) {
                    connection->mark_disconnected();
                    status = VI_ERROR_RSRC_NFOUND;
                    return nullptr;
                }
                connections_[identity] = connection;
                auto& device_entries = device_connections_[device];
                device_entries.erase(
                    std::remove_if(device_entries.begin(),
                                   device_entries.end(),
                                   [](const auto& weak) {
                                       return weak.expired();
                                   }),
                    device_entries.end());
                device_entries.push_back(connection);
            }
            status = VI_SUCCESS;
            return std::make_unique<LibusbTransport>(std::move(connection),
                                                     *selected);
        }
        status = deferred_status;
        return nullptr;
    }

    LibusbProvider(std::shared_ptr<LibusbRuntime> runtime,
                   ViStatus initialization_status)
        : runtime_(std::move(runtime)),
          initialization_status_(initialization_status) {}

    void register_hotplug() {
        if (runtime_) {
            hotplug_registered_ = runtime_->register_hotplug(
                &LibusbProvider::hotplug_callback, this, hotplug_handle_);
        }
    }

    static int LIBUSB_CALL hotplug_callback(libusb_context* context,
                                             libusb_device* device,
                                             libusb_hotplug_event event,
                                             void* user_data) {
        static_cast<void>(context);
        auto* provider = static_cast<LibusbProvider*>(user_data);
        if (event == LIBUSB_HOTPLUG_EVENT_DEVICE_LEFT) {
            provider->device_left(device);
        } else if (event == LIBUSB_HOTPLUG_EVENT_DEVICE_ARRIVED) {
            provider->device_arrived(device);
        }
        return 0;
    }

    void device_left(libusb_device* device) noexcept {
        std::vector<std::shared_ptr<LibusbConnection>> connections;
        {
            std::lock_guard lock(mutex_);
            departed_devices_.insert(device);
            const auto found = device_connections_.find(device);
            if (found == device_connections_.end()) {
                return;
            }
            for (const auto& weak : found->second) {
                if (auto connection = weak.lock()) {
                    connections.push_back(std::move(connection));
                }
            }
            device_connections_.erase(found);
        }
        // The hotplug callback intentionally performs no blocking descriptor,
        // handle, or interface operations. Pending transfers are completed by
        // libusb with NO_DEVICE; new transfers fail from this atomic marker.
        for (const auto& connection : connections) {
            connection->mark_disconnected();
        }
    }

    void device_arrived(libusb_device* device) noexcept {
        std::lock_guard lock(mutex_);
        departed_devices_.erase(device);
    }

    std::shared_ptr<LibusbRuntime> runtime_;
    ViStatus initialization_status_{VI_ERROR_SYSTEM_ERROR};
    std::mutex open_mutex_;
    std::mutex mutex_;
    std::map<std::string, std::weak_ptr<LibusbConnection>> connections_;
    std::map<libusb_device*,
             std::vector<std::weak_ptr<LibusbConnection>>>
        device_connections_;
    std::set<libusb_device*> departed_devices_;
    UsbInterfaceArbiter arbiter_;
    bool hotplug_registered_{false};
    libusb_hotplug_callback_handle hotplug_handle_{};
};

}  // namespace
#endif

void ensure_libusb_provider_registered() {
#if WRVISA_HAS_LIBUSB
    static UsbProviderRegistration registration =
        register_usb_provider(LibusbProvider::create());
    static_cast<void>(registration);
#endif
}

}  // namespace wrvisa
