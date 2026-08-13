#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <future>
#include <latch>
#include <memory>
#include <mutex>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "backends/usb/usb_provider.h"
#include "backends/usb/libusb_provider.h"
#include "backends/usb/usbtmc_protocol.h"
#include "core/handle_table.h"
#include "core/objects.h"
#include "test_support.h"
#include "visa.h"
#include "webreal_visa_ext.h"

namespace {

constexpr const char* kInstrResource =
    "USB7::0x1209::0x0001::WRVISA-SIM-0001::0::INSTR";
constexpr const char* kRawResource =
    "USB7::0x1209::0x0001::WRVISA-SIM-0001::0::RAW";

struct DeviceState {
    std::mutex mutex;
    std::condition_variable condition;
    bool connected{true};
    std::uint64_t generation{1};
    std::size_t physical_claims{0};
    std::size_t releases{0};
    std::size_t usbtmc_reads_waiting{0};
    std::size_t raw_reads_waiting{0};
};

class FakeUsbTransport final : public wrvisa::UsbTransport {
public:
    FakeUsbTransport(std::shared_ptr<DeviceState> state,
                     std::uint64_t generation,
                     std::shared_ptr<wrvisa::UsbInterfaceLease> lease)
        : state_(std::move(state)),
          generation_(generation),
          lease_(std::move(lease)) {}

    wrvisa::UsbInterfaceInfo interface_info() const noexcept override {
        return {0, 0x02, 0x81, 0x83, 64, true};
    }

    ViStatus bulk_out(wrvisa::Operation& operation,
                      std::span<const std::uint8_t> data,
                      std::size_t& transferred) override {
        transferred = 0;
        if (operation.completed()) {
            return operation.result();
        }
        std::lock_guard lock(state_->mutex);
        const auto ready = ready_status();
        if (ready < VI_SUCCESS) {
            return ready;
        }

        wrvisa::usbtmc::DevDepMessage message;
        if (wrvisa::usbtmc::decode_dev_dep_msg_out(data, message)) {
            const std::string command(message.payload.begin(),
                                      message.payload.end());
            response_ = command == "*IDN?\n"
                            ? "WEBREAL,USB-TMC-SIM,0001,0.5\n"
                            : std::string{};
            transferred = data.size();
            return VI_SUCCESS;
        }

        std::uint8_t trigger_tag = 0;
        if (wrvisa::usbtmc::decode_usb488_trigger(data, trigger_tag)) {
            static_cast<void>(trigger_tag);
            transferred = data.size();
            return VI_SUCCESS;
        }

        std::uint8_t tag = 0;
        std::uint32_t requested = 0;
        bool termchar_enabled = false;
        std::uint8_t termchar = 0;
        if (!wrvisa::usbtmc::decode_request_dev_dep_msg_in(
                data, tag, requested, termchar_enabled, termchar)) {
            return VI_ERROR_IO;
        }
        static_cast<void>(termchar_enabled);
        static_cast<void>(termchar);
        if (!response_.empty()) {
            const auto amount = std::min<std::size_t>(requested, response_.size());
            const auto payload = std::span<const std::uint8_t>(
                reinterpret_cast<const std::uint8_t*>(response_.data()), amount);
            const bool end = amount == response_.size();
            auto frame = wrvisa::usbtmc::encode_dev_dep_msg_in(tag, payload, end);
            if (!frame) {
                return VI_ERROR_IO;
            }
            pending_ = std::move(*frame);
            response_.erase(0, amount);
        }
        transferred = data.size();
        state_->condition.notify_all();
        return VI_SUCCESS;
    }

    ViStatus bulk_in(wrvisa::Operation& operation, std::size_t maximum_size,
                     std::vector<std::uint8_t>& data) override {
        std::unique_lock lock(state_->mutex);
        struct WaitingRead {
            explicit WaitingRead(DeviceState& value) : state(value) {
                ++state.usbtmc_reads_waiting;
                state.condition.notify_all();
            }
            ~WaitingRead() { --state.usbtmc_reads_waiting; }
            DeviceState& state;
        } waiting(*state_);
        const auto predicate = [&] {
            return !pending_.empty() || abort_in_drain_ ||
                   ready_status() < VI_SUCCESS || operation.completed();
        };
        if (operation.deadline() == wrvisa::Operation::Clock::time_point::max()) {
            state_->condition.wait(lock, predicate);
        } else if (!state_->condition.wait_until(lock, operation.deadline(),
                                                 predicate)) {
            static_cast<void>(operation.request_timeout());
        }
        if (operation.completed()) {
            return operation.result();
        }
        const auto ready = ready_status();
        if (ready < VI_SUCCESS) {
            return ready;
        }
        if (abort_in_drain_) {
            abort_in_drain_ = false;
            data.clear();
            return VI_SUCCESS;
        }
        if (pending_.size() > maximum_size) {
            return VI_ERROR_IO;
        }
        data = std::move(pending_);
        pending_.clear();
        return VI_SUCCESS;
    }

    ViStatus control_in(wrvisa::Operation& operation,
                        const wrvisa::UsbControlRequest& request,
                        std::size_t response_size,
                        std::vector<std::uint8_t>& response) override {
        if (operation.completed()) {
            return operation.result();
        }
        std::lock_guard lock(state_->mutex);
        const auto ready = ready_status();
        if (ready < VI_SUCCESS) {
            return ready;
        }
        response.clear();
        switch (request.request) {
            case 1:
            case 3:
                response = {wrvisa::usbtmc::kStatusSuccess,
                            static_cast<std::uint8_t>(request.value)};
                if (request.request == 3) {
                    abort_in_drain_ = true;
                    state_->condition.notify_all();
                }
                break;
            case 2:
            case 4:
                response = {wrvisa::usbtmc::kStatusSuccess, 0, 0, 0,
                            0, 0, 0, 0};
                break;
            case 5:
                response_.clear();
                pending_.clear();
                response = {wrvisa::usbtmc::kStatusSuccess};
                break;
            case 6:
                response = {wrvisa::usbtmc::kStatusSuccess, 0};
                break;
            case 7:
                response.assign(24, 0);
                response[0] = wrvisa::usbtmc::kStatusSuccess;
                response[3] = 1;
                response[5] = 1;
                response[13] = 1;
                response[14] = 5;
                response[15] = 5;
                break;
            case 128: {
                const auto tag = static_cast<std::uint8_t>(request.value);
                response = {wrvisa::usbtmc::kStatusSuccess, tag, 0};
                interrupt_pending_ = {
                    static_cast<std::uint8_t>(0x80u | tag), 0x42};
                break;
            }
            default:
                return VI_ERROR_NSUP_OPER;
        }
        return response.size() == response_size ? VI_SUCCESS : VI_ERROR_IO;
    }

    ViStatus interrupt_in(wrvisa::Operation& operation,
                          std::size_t maximum_size,
                          std::vector<std::uint8_t>& data) override {
        if (operation.completed()) {
            return operation.result();
        }
        std::lock_guard lock(state_->mutex);
        const auto ready = ready_status();
        if (ready < VI_SUCCESS) {
            return ready;
        }
        if (interrupt_pending_.empty() ||
            interrupt_pending_.size() > maximum_size) {
            return VI_ERROR_IO;
        }
        data = std::move(interrupt_pending_);
        interrupt_pending_.clear();
        return VI_SUCCESS;
    }

    ViStatus clear_halt(wrvisa::Operation& operation,
                        std::uint8_t endpoint_address) override {
        if (operation.completed()) {
            return operation.result();
        }
        std::lock_guard lock(state_->mutex);
        return endpoint_address == 0x02 ? ready_status() : VI_ERROR_IO;
    }

    void cancel() noexcept override { state_->condition.notify_all(); }

    void close() noexcept override {
        {
            std::lock_guard lock(state_->mutex);
            closed_ = true;
        }
        state_->condition.notify_all();
        lease_.reset();
    }

private:
    ViStatus ready_status() const noexcept {
        return closed_ || !state_->connected ||
                       generation_ != state_->generation
                   ? VI_ERROR_CONN_LOST
                   : VI_SUCCESS;
    }

    std::shared_ptr<DeviceState> state_;
    std::uint64_t generation_;
    std::shared_ptr<wrvisa::UsbInterfaceLease> lease_;
    bool closed_{false};
    std::string response_;
    std::vector<std::uint8_t> pending_;
    std::vector<std::uint8_t> interrupt_pending_;
    bool abort_in_drain_{false};
};

class FakeRawUsbTransport final : public wrvisa::UsbTransport {
public:
    FakeRawUsbTransport(std::shared_ptr<DeviceState> state,
                        std::uint64_t generation,
                        std::shared_ptr<wrvisa::UsbInterfaceLease> lease)
        : state_(std::move(state)),
          generation_(generation),
          lease_(std::move(lease)) {}

    wrvisa::UsbInterfaceInfo interface_info() const noexcept override {
        return {0, 0x02, 0x81, 0x83, 64, false, 0, 0x04};
    }

    ViStatus bulk_out(wrvisa::Operation& operation,
                      std::span<const std::uint8_t> data,
                      std::size_t& transferred) override {
        transferred = 0;
        if (operation.completed()) {
            return operation.result();
        }
        std::lock_guard lock(state_->mutex);
        const auto status = ready_status();
        if (status < VI_SUCCESS) {
            return status;
        }
        pending_.insert(pending_.end(), data.begin(), data.end());
        transferred = data.size();
        state_->condition.notify_all();
        return VI_SUCCESS;
    }

    ViStatus bulk_in(wrvisa::Operation& operation, std::size_t maximum_size,
                     std::vector<std::uint8_t>& data) override {
        std::unique_lock lock(state_->mutex);
        struct WaitingRead {
            explicit WaitingRead(DeviceState& value) : state(value) {
                ++state.raw_reads_waiting;
                state.condition.notify_all();
            }
            ~WaitingRead() { --state.raw_reads_waiting; }
            DeviceState& state;
        } waiting(*state_);
        const auto predicate = [&] {
            return !pending_.empty() || ready_status() < VI_SUCCESS ||
                   operation.completed();
        };
        if (operation.deadline() == wrvisa::Operation::Clock::time_point::max()) {
            state_->condition.wait(lock, predicate);
        } else if (!state_->condition.wait_until(lock, operation.deadline(),
                                                 predicate)) {
            static_cast<void>(operation.request_timeout());
        }
        if (operation.completed()) {
            return operation.result();
        }
        const auto status = ready_status();
        if (status < VI_SUCCESS) {
            return status;
        }
        const auto amount = std::min(maximum_size, pending_.size());
        data.assign(pending_.begin(), pending_.begin() +
                                          static_cast<std::ptrdiff_t>(amount));
        pending_.erase(pending_.begin(), pending_.begin() +
                                             static_cast<std::ptrdiff_t>(amount));
        return VI_SUCCESS;
    }

    ViStatus control_in(wrvisa::Operation& operation,
                        const wrvisa::UsbControlRequest& request,
                        std::size_t response_size,
                        std::vector<std::uint8_t>& response) override {
        if (operation.completed()) {
            return operation.result();
        }
        std::lock_guard lock(state_->mutex);
        const auto status = ready_status();
        if (status < VI_SUCCESS) {
            return status;
        }
        response.resize(response_size);
        for (std::size_t index = 0; index < response.size(); ++index) {
            response[index] = static_cast<std::uint8_t>(request.request + index);
        }
        return VI_SUCCESS;
    }

    ViStatus control_out(wrvisa::Operation& operation,
                         const wrvisa::UsbControlRequest& request,
                         std::span<const std::uint8_t> data,
                         std::size_t& transferred) override {
        static_cast<void>(request);
        if (operation.completed()) {
            transferred = 0;
            return operation.result();
        }
        std::lock_guard lock(state_->mutex);
        const auto status = ready_status();
        if (status < VI_SUCCESS) {
            transferred = 0;
            return status;
        }
        control_out_.assign(data.begin(), data.end());
        transferred = data.size();
        return VI_SUCCESS;
    }

    ViStatus clear_halt(wrvisa::Operation& operation,
                        std::uint8_t endpoint_address) override {
        if (operation.completed()) {
            return operation.result();
        }
        return endpoint_address == 0x02 || endpoint_address == 0x81
                   ? VI_SUCCESS
                   : VI_ERROR_INV_PARAMETER;
    }

    void cancel() noexcept override { state_->condition.notify_all(); }

    void close() noexcept override {
        {
            std::lock_guard lock(state_->mutex);
            closed_ = true;
        }
        state_->condition.notify_all();
        lease_.reset();
    }

private:
    ViStatus ready_status() const noexcept {
        return closed_ || !state_->connected ||
                       generation_ != state_->generation
                   ? VI_ERROR_CONN_LOST
                   : VI_SUCCESS;
    }

    std::shared_ptr<DeviceState> state_;
    std::uint64_t generation_;
    std::shared_ptr<wrvisa::UsbInterfaceLease> lease_;
    bool closed_{false};
    std::vector<std::uint8_t> pending_;
    std::vector<std::uint8_t> control_out_;
};

class FakeUsbProvider final : public wrvisa::UsbProvider {
public:
    std::vector<std::string> discover() override {
        std::lock_guard lock(state_->mutex);
        if (!state_->connected) {
            return {};
        }
        return {kRawResource, kInstrResource,
                "usb7::0x1209::0x0001::WRVISA-SIM-0001::0::instr",
                "not-a-resource"};
    }

    std::unique_ptr<wrvisa::UsbTransport> open(
        const wrvisa::ResourceDescriptor& resource, ViUInt32 timeout,
        ViStatus& status) override {
        static_cast<void>(timeout);
        if (resource.kind != wrvisa::ResourceKind::usb_instr ||
            resource.canonical_name != kInstrResource) {
            status = VI_ERROR_RSRC_NFOUND;
            return nullptr;
        }
        std::uint64_t generation = 0;
        {
            std::lock_guard lock(state_->mutex);
            if (!state_->connected) {
                status = VI_ERROR_RSRC_NFOUND;
                return nullptr;
            }
            generation = state_->generation;
        }
        auto state = state_;
        auto lease = arbiter_.acquire(
            identity(generation),
            [state, generation] {
                std::lock_guard lock(state->mutex);
                if (!state->connected || state->generation != generation) {
                    return VI_ERROR_CONN_LOST;
                }
                ++state->physical_claims;
                return VI_SUCCESS;
            },
            [state] {
                std::lock_guard lock(state->mutex);
                if (state->physical_claims != 0) {
                    --state->physical_claims;
                }
                ++state->releases;
            },
            status);
        if (!lease) {
            return nullptr;
        }
        return std::make_unique<FakeUsbTransport>(state_, generation,
                                                  std::move(lease));
    }

    std::unique_ptr<wrvisa::UsbTransport> open_raw(
        const wrvisa::ResourceDescriptor& resource,
        const wrvisa::UsbRawConfiguration& configuration, ViUInt32 timeout,
        ViStatus& status) override {
        static_cast<void>(timeout);
        if (resource.kind != wrvisa::ResourceKind::usb_raw ||
            resource.canonical_name != kRawResource ||
            configuration.alternate_setting != 0 ||
            configuration.read_transfer_type !=
                wrvisa::UsbTransferType::bulk ||
            configuration.read_endpoint != 0x81 ||
            configuration.write_transfer_type !=
                wrvisa::UsbTransferType::bulk ||
            configuration.write_endpoint != 0x02) {
            status = VI_ERROR_RSRC_NFOUND;
            return nullptr;
        }
        std::uint64_t generation = 0;
        {
            std::lock_guard lock(state_->mutex);
            if (!state_->connected) {
                status = VI_ERROR_RSRC_NFOUND;
                return nullptr;
            }
            generation = state_->generation;
        }
        auto state = state_;
        auto lease = arbiter_.acquire(
            identity(generation),
            [state, generation] {
                std::lock_guard lock(state->mutex);
                if (!state->connected || state->generation != generation) {
                    return VI_ERROR_CONN_LOST;
                }
                ++state->physical_claims;
                return VI_SUCCESS;
            },
            [state] {
                std::lock_guard lock(state->mutex);
                if (state->physical_claims != 0) {
                    --state->physical_claims;
                }
                ++state->releases;
            },
            status);
        if (!lease) {
            return nullptr;
        }
        return std::make_unique<FakeRawUsbTransport>(state_, generation,
                                                     std::move(lease));
    }

    void disconnect() {
        std::string old_identity;
        {
            std::lock_guard lock(state_->mutex);
            old_identity = identity(state_->generation);
            state_->connected = false;
            ++state_->generation;
        }
        arbiter_.invalidate(old_identity);
        state_->condition.notify_all();
    }

    void reconnect() {
        {
            std::lock_guard lock(state_->mutex);
            state_->connected = true;
        }
        state_->condition.notify_all();
    }

    std::size_t physical_claims() const {
        std::lock_guard lock(state_->mutex);
        return state_->physical_claims;
    }

    std::size_t releases() const {
        std::lock_guard lock(state_->mutex);
        return state_->releases;
    }

    bool wait_for_read(bool raw, std::chrono::milliseconds timeout) {
        std::unique_lock lock(state_->mutex);
        return state_->condition.wait_for(lock, timeout, [&] {
            return raw ? state_->raw_reads_waiting != 0
                       : state_->usbtmc_reads_waiting != 0;
        });
    }

private:
    static std::string identity(std::uint64_t generation) {
        return "USB0::0x1209::0x0001::WRVISA-SIM-0001::0#" +
               std::to_string(generation);
    }

    std::shared_ptr<DeviceState> state_{std::make_shared<DeviceState>()};
    wrvisa::UsbInterfaceArbiter arbiter_;
};

class FailingDiscoveryProvider final : public wrvisa::UsbProvider {
public:
    std::vector<std::string> discover() override {
        throw std::runtime_error("simulated discovery failure");
    }

    std::unique_ptr<wrvisa::UsbTransport> open(
        const wrvisa::ResourceDescriptor& resource, ViUInt32 timeout,
        ViStatus& status) override {
        static_cast<void>(resource);
        static_cast<void>(timeout);
        status = VI_ERROR_RSRC_NFOUND;
        return nullptr;
    }
};

class DiagnosticOpenProvider final : public wrvisa::UsbProvider {
public:
    std::vector<std::string> discover() override { return {}; }

    std::unique_ptr<wrvisa::UsbTransport> open(
        const wrvisa::ResourceDescriptor& resource, ViUInt32 timeout,
        ViStatus& status) override {
        static_cast<void>(resource);
        static_cast<void>(timeout);
        status = VI_ERROR_IO;
        return nullptr;
    }

    std::unique_ptr<wrvisa::UsbTransport> open_raw(
        const wrvisa::ResourceDescriptor& resource,
        const wrvisa::UsbRawConfiguration& configuration, ViUInt32 timeout,
        ViStatus& status) override {
        static_cast<void>(resource);
        static_cast<void>(configuration);
        static_cast<void>(timeout);
        status = VI_ERROR_IO;
        return nullptr;
    }
};

class BrokenOpenProvider final : public wrvisa::UsbProvider {
public:
    std::vector<std::string> discover() override { return {}; }

    std::unique_ptr<wrvisa::UsbTransport> open(
        const wrvisa::ResourceDescriptor& resource, ViUInt32 timeout,
        ViStatus& status) override {
        static_cast<void>(resource);
        static_cast<void>(timeout);
        status = VI_SUCCESS;
        return nullptr;
    }
};

std::shared_ptr<wrvisa::SessionObject> session_object(ViSession session) {
    auto object = wrvisa::get_handle<wrvisa::SessionObject>(
        session, wrvisa::ObjectType::session);
    CHECK(object != nullptr);
    return object;
}

std::vector<std::string> find_usb(ViSession rm) {
    ViFindList list = VI_NULL;
    ViUInt32 count = 0;
    std::array<ViChar, VI_FIND_BUFLEN> description{};
    const auto status = viFindRsrc(rm, "USB?*", &list, &count,
                                   description.data());
    if (status == VI_ERROR_RSRC_NFOUND) {
        return {};
    }
    CHECK(status == VI_SUCCESS);
    std::vector<std::string> resources{description.data()};
    while (resources.size() < count) {
        CHECK(viFindNext(list, description.data()) == VI_SUCCESS);
        resources.emplace_back(description.data());
    }
    CHECK(viFindNext(list, description.data()) == VI_ERROR_RSRC_NFOUND);
    CHECK(viClose(list) == VI_SUCCESS);
    return resources;
}

void query_identity(ViSession session) {
    constexpr std::string_view query = "*IDN?\n";
    ViUInt32 count = 0;
    CHECK(viWrite(session,
                  reinterpret_cast<ViConstBuf>(query.data()),
                  static_cast<ViUInt32>(query.size()), &count) == VI_SUCCESS);
    CHECK(count == query.size());
    CHECK(viSetAttribute(session, VI_ATTR_TERMCHAR, '\n') == VI_SUCCESS);
    CHECK(viSetAttribute(session, VI_ATTR_TERMCHAR_EN, VI_TRUE) == VI_SUCCESS);
    std::array<ViByte, 128> buffer{};
    count = 0;
    CHECK(viRead(session, buffer.data(), static_cast<ViUInt32>(buffer.size()),
                 &count) == VI_SUCCESS_TERM_CHAR);
    CHECK(std::string(reinterpret_cast<const char*>(buffer.data()), count) ==
          "WEBREAL,USB-TMC-SIM,0001,0.5\n");
}

}  // namespace

int main() {
    using namespace std::chrono_literals;
    using wrvisa::register_usb_provider;

    const auto platform_resources = wrvisa::discover_usb_resources();

    CHECK(!register_usb_provider(nullptr));
    auto failing_registration =
        register_usb_provider(std::make_shared<FailingDiscoveryProvider>());
    auto diagnostic_registration =
        register_usb_provider(std::make_shared<DiagnosticOpenProvider>());
    auto provider = std::make_shared<FakeUsbProvider>();
    auto registration = register_usb_provider(provider);
    CHECK(failing_registration);
    CHECK(registration);

    ViSession rm = VI_NULL;
    CHECK(viOpenDefaultRM(&rm) == VI_SUCCESS);
    const auto resources = find_usb(rm);
    auto expected_resources = platform_resources;
    expected_resources.emplace_back(kInstrResource);
    expected_resources.emplace_back(kRawResource);
    std::sort(expected_resources.begin(), expected_resources.end());
    expected_resources.erase(
        std::unique(expected_resources.begin(), expected_resources.end()),
        expected_resources.end());
    CHECK(resources == expected_resources);

    ViSession raw = VI_NULL;
    CHECK(viOpen(rm, kRawResource, VI_NO_LOCK, 1000, &raw) ==
          VI_ERROR_INTF_NUM_NCONFIG);
    CHECK(raw == VI_NULL);

    wrvisa_usb_raw_config_v1 raw_config{};
    raw_config.struct_size = sizeof(raw_config);
    raw_config.abi_major = WRVISA_USB_RAW_ABI_MAJOR;
    raw_config.abi_minor = WRVISA_USB_RAW_ABI_MINOR;
    raw_config.read_transfer_type = WRVISA_USB_TRANSFER_BULK;
    raw_config.read_endpoint = 0x81;
    raw_config.write_transfer_type = WRVISA_USB_TRANSFER_BULK;
    raw_config.write_endpoint = 0x02;
    CHECK(wrvisaSetUsbRawConfig(VI_NULL, kRawResource, &raw_config) ==
          VI_ERROR_INV_OBJECT);
    CHECK(wrvisaSetUsbRawConfig(rm, nullptr, &raw_config) ==
          VI_ERROR_INV_PARAMETER);
    CHECK(wrvisaSetUsbRawConfig(rm, kRawResource, nullptr) ==
          VI_ERROR_INV_PARAMETER);
    auto bad_config = raw_config;
    bad_config.struct_size--;
    CHECK(wrvisaSetUsbRawConfig(rm, kRawResource, &bad_config) ==
          VI_ERROR_INV_SIZE);
    bad_config = raw_config;
    ++bad_config.abi_major;
    CHECK(wrvisaSetUsbRawConfig(rm, kRawResource, &bad_config) ==
          VI_ERROR_NSUP_OPER);
    bad_config = raw_config;
    bad_config.read_endpoint = 0x01;
    CHECK(wrvisaSetUsbRawConfig(rm, kRawResource, &bad_config) ==
          VI_ERROR_INV_PARAMETER);
    bad_config = raw_config;
    bad_config.read_endpoint = 0x91;
    CHECK(wrvisaSetUsbRawConfig(rm, kRawResource, &bad_config) ==
          VI_ERROR_INV_PARAMETER);
    bad_config = raw_config;
    bad_config.reserved[0] = 1;
    CHECK(wrvisaSetUsbRawConfig(rm, kRawResource, &bad_config) ==
          VI_ERROR_INV_PARAMETER);
    CHECK(wrvisaSetUsbRawConfig(rm, kInstrResource, &raw_config) ==
          VI_ERROR_NSUP_OPER);
    CHECK(wrvisaSetUsbRawConfig(rm, kRawResource, &raw_config) == VI_SUCCESS);
    CHECK(viOpen(rm, kRawResource, VI_NO_LOCK, 1000, &raw) == VI_SUCCESS);

    ViSession first = VI_NULL;
    ViSession second = VI_NULL;
    CHECK(viOpen(rm, kInstrResource, VI_NO_LOCK, 1000, &first) == VI_SUCCESS);
    CHECK(viOpen(rm, kInstrResource, VI_NO_LOCK, 1000, &second) == VI_SUCCESS);
    CHECK(provider->physical_claims() == 1);

    constexpr std::string_view raw_payload = "RAW\nTAIL";
    ViUInt32 raw_count = 0;
    CHECK(viWrite(raw, reinterpret_cast<ViConstBuf>(raw_payload.data()),
                  static_cast<ViUInt32>(raw_payload.size()), &raw_count) ==
          VI_SUCCESS);
    CHECK(raw_count == raw_payload.size());
    CHECK(viSetAttribute(raw, VI_ATTR_TERMCHAR, '\n') == VI_SUCCESS);
    CHECK(viSetAttribute(raw, VI_ATTR_TERMCHAR_EN, VI_TRUE) == VI_SUCCESS);
    std::array<ViByte, 32> raw_buffer{};
    raw_count = 0;
    CHECK(viRead(raw, raw_buffer.data(),
                 static_cast<ViUInt32>(raw_buffer.size()), &raw_count) ==
          VI_SUCCESS_TERM_CHAR);
    CHECK(std::string(reinterpret_cast<const char*>(raw_buffer.data()),
                      raw_count) == "RAW\n");
    CHECK(viSetAttribute(raw, VI_ATTR_TERMCHAR_EN, VI_FALSE) == VI_SUCCESS);
    raw_count = 0;
    CHECK(viRead(raw, raw_buffer.data(),
                 static_cast<ViUInt32>(raw_buffer.size()), &raw_count) ==
          VI_SUCCESS);
    CHECK(std::string(reinterpret_cast<const char*>(raw_buffer.data()),
                      raw_count) == "TAIL");

    wrvisa_usb_control_request_v1 control{};
    control.struct_size = sizeof(control);
    control.abi_major = WRVISA_USB_RAW_ABI_MAJOR;
    control.abi_minor = WRVISA_USB_RAW_ABI_MINOR;
    control.request_type = 0xC0;
    control.request = 0x30;
    raw_count = 99;
    CHECK(wrvisaUsbControlTransfer(raw, nullptr, raw_buffer.data(), 4,
                                   &raw_count) == VI_ERROR_INV_PARAMETER);
    CHECK(raw_count == 0);
    raw_count = 99;
    CHECK(wrvisaUsbControlTransfer(raw, &control, nullptr, 4, &raw_count) ==
          VI_ERROR_INV_PARAMETER);
    CHECK(raw_count == 0);
    CHECK(wrvisaUsbControlTransfer(raw, &control, raw_buffer.data(), 4,
                                   nullptr) == VI_ERROR_INV_PARAMETER);
    raw_count = 99;
    CHECK(wrvisaUsbControlTransfer(
              raw, &control, raw_buffer.data(),
              static_cast<ViUInt32>(UINT16_MAX) + 1u, &raw_count) ==
          VI_ERROR_INV_SIZE);
    CHECK(raw_count == 0);
    auto bad_control = control;
    --bad_control.struct_size;
    CHECK(wrvisaUsbControlTransfer(raw, &bad_control, nullptr, 0,
                                   &raw_count) == VI_ERROR_INV_SIZE);
    bad_control = control;
    ++bad_control.abi_minor;
    CHECK(wrvisaUsbControlTransfer(raw, &bad_control, nullptr, 0,
                                   &raw_count) == VI_ERROR_NSUP_OPER);
    bad_control = control;
    bad_control.reserved[0] = 1;
    CHECK(wrvisaUsbControlTransfer(raw, &bad_control, nullptr, 0,
                                   &raw_count) == VI_ERROR_INV_PARAMETER);
    bad_control = control;
    bad_control.request_type = 0x60;
    CHECK(wrvisaUsbControlTransfer(raw, &bad_control, nullptr, 0,
                                   &raw_count) == VI_ERROR_INV_PARAMETER);
    bad_control = control;
    bad_control.request_type = 0xC4;
    CHECK(wrvisaUsbControlTransfer(raw, &bad_control, nullptr, 0,
                                   &raw_count) == VI_ERROR_INV_PARAMETER);
    raw_count = 99;
    CHECK(wrvisaUsbControlTransfer(raw, &control, raw_buffer.data(), 4,
                                   &raw_count) == VI_SUCCESS);
    CHECK(raw_count == 4);
    CHECK(raw_buffer[0] == 0x30 && raw_buffer[3] == 0x33);
    control.request_type = 0x40;
    control.request = 0x31;
    raw_count = 0;
    CHECK(wrvisaUsbControlTransfer(raw, &control, raw_buffer.data(), 4,
                                   &raw_count) == VI_SUCCESS);
    CHECK(raw_count == 4);
    CHECK(wrvisaUsbControlTransfer(first, &control, raw_buffer.data(), 4,
                                   &raw_count) == VI_ERROR_NSUP_OPER);

    CHECK(viSetAttribute(raw, VI_ATTR_TMO_VALUE, 5000) == VI_SUCCESS);
    auto raw_object = session_object(raw);
    raw_count = 99;
    auto canceled_raw_read = std::async(std::launch::async, [&] {
        return viRead(raw, raw_buffer.data(),
                      static_cast<ViUInt32>(raw_buffer.size()), &raw_count);
    });
    CHECK(provider->wait_for_read(true, 1s));
    CHECK(viTerminate(raw, VI_NULL, VI_NULL) == VI_SUCCESS);
    CHECK(canceled_raw_read.wait_for(2s) == std::future_status::ready);
    CHECK(canceled_raw_read.get() == VI_ERROR_ABORT);
    CHECK(raw_count == 0);
    CHECK(viClear(raw) == VI_SUCCESS);
    CHECK(viClose(raw) == VI_SUCCESS);
    raw_object.reset();
    CHECK(provider->physical_claims() == 1);
    query_identity(first);

    ViUInt16 status_byte = 0;
    CHECK(viReadSTB(second, &status_byte) == VI_SUCCESS);
    CHECK(status_byte == 0x42);
    CHECK(viAssertTrigger(second, VI_TRIG_PROT_DEFAULT) == VI_SUCCESS);
    CHECK(viClear(second) == VI_SUCCESS);
    CHECK(viClose(first) == VI_SUCCESS);
    CHECK(provider->physical_claims() == 1);
    query_identity(second);

    CHECK(viSetAttribute(second, VI_ATTR_TMO_VALUE, 5000) == VI_SUCCESS);
    auto second_object = session_object(second);
    std::array<ViByte, 16> canceled_buffer{};
    ViUInt32 canceled_count = 99;
    auto canceled_read = std::async(std::launch::async, [&] {
        return viRead(second, canceled_buffer.data(),
                      static_cast<ViUInt32>(canceled_buffer.size()),
                      &canceled_count);
    });
    CHECK(provider->wait_for_read(false, 1s));
    CHECK(viTerminate(second, VI_NULL, VI_NULL) == VI_SUCCESS);
    // USBTMC cancellation may consume the full 500 ms abort budget and then
    // the independent 500 ms clear fallback before returning the original
    // abort status. Leave scheduling margin around that specified recovery.
    CHECK(canceled_read.wait_for(2s) == std::future_status::ready);
    CHECK(canceled_read.get() == VI_ERROR_ABORT);
    CHECK(canceled_count == 0);
    query_identity(second);
    CHECK(viClose(second) == VI_SUCCESS);
    second_object.reset();
    CHECK(provider->physical_claims() == 0);
    CHECK(provider->releases() == 1);

    constexpr std::size_t kConcurrentSessions = 8;
    std::latch start_gate(kConcurrentSessions + 1);
    std::vector<std::future<std::pair<ViStatus, ViSession>>> opens;
    opens.reserve(kConcurrentSessions);
    for (std::size_t index = 0; index < kConcurrentSessions; ++index) {
        opens.push_back(std::async(std::launch::async, [&] {
            start_gate.arrive_and_wait();
            ViSession session = VI_NULL;
            const auto status = viOpen(rm, kInstrResource, VI_NO_LOCK, 1000,
                                       &session);
            return std::pair{status, session};
        }));
    }
    start_gate.arrive_and_wait();
    std::vector<ViSession> concurrent_sessions;
    concurrent_sessions.reserve(kConcurrentSessions);
    for (auto& open : opens) {
        const auto [status, session] = open.get();
        CHECK(status == VI_SUCCESS);
        concurrent_sessions.push_back(session);
    }
    CHECK(provider->physical_claims() == 1);
    for (const auto session : concurrent_sessions) {
        CHECK(viClose(session) == VI_SUCCESS);
    }
    CHECK(provider->physical_claims() == 0);
    CHECK(provider->releases() == 2);

    diagnostic_registration.reset();

    ViSession unplugged = VI_NULL;
    CHECK(viOpen(rm, kInstrResource, VI_NO_LOCK, 1000, &unplugged) == VI_SUCCESS);
    CHECK(viSetAttribute(unplugged, VI_ATTR_TMO_VALUE, 5000) == VI_SUCCESS);
    auto unplugged_object = session_object(unplugged);
    std::array<ViByte, 16> unplugged_buffer{};
    ViUInt32 unplugged_count = 77;
    auto unplugged_read = std::async(std::launch::async, [&] {
        return viRead(unplugged, unplugged_buffer.data(),
                      static_cast<ViUInt32>(unplugged_buffer.size()),
                      &unplugged_count);
    });
    CHECK(provider->wait_for_read(false, 1s));
    provider->disconnect();
    CHECK(unplugged_read.wait_for(2s) == std::future_status::ready);
    CHECK(unplugged_read.get() == VI_ERROR_CONN_LOST);
    CHECK(unplugged_count == 0);
    CHECK(provider->physical_claims() == 0);
    CHECK(provider->releases() == 3);
    CHECK(find_usb(rm) == resources);

    ViSession disconnected_rm = VI_NULL;
    CHECK(viOpenDefaultRM(&disconnected_rm) == VI_SUCCESS);
    CHECK(find_usb(disconnected_rm) == platform_resources);
    ViSession missing = VI_NULL;
    CHECK(viOpen(rm, kInstrResource, VI_NO_LOCK, 1000, &missing) ==
          VI_ERROR_RSRC_NFOUND);

    provider->reconnect();
    ViSession reconnected = VI_NULL;
    CHECK(viOpen(rm, kInstrResource, VI_NO_LOCK, 1000, &reconnected) ==
          VI_SUCCESS);
    query_identity(reconnected);
    constexpr std::string_view query = "*IDN?\n";
    ViUInt32 stale_count = 99;
    CHECK(viWrite(unplugged,
                  reinterpret_cast<ViConstBuf>(query.data()),
                  static_cast<ViUInt32>(query.size()), &stale_count) ==
          VI_ERROR_CONN_LOST);
    CHECK(stale_count == 0);

    CHECK(viClose(unplugged) == VI_SUCCESS);
    unplugged_object.reset();
    CHECK(viClose(reconnected) == VI_SUCCESS);
    CHECK(provider->physical_claims() == 0);
    CHECK(provider->releases() == 4);
    CHECK(viClose(disconnected_rm) == VI_SUCCESS);

    registration.reset();
    failing_registration.reset();
    auto broken_registration =
        register_usb_provider(std::make_shared<BrokenOpenProvider>());
    ViSession broken = VI_NULL;
    CHECK(viOpen(rm, kInstrResource, VI_NO_LOCK, 1000, &broken) ==
          VI_ERROR_SYSTEM_ERROR);
    broken_registration.reset();
    ViSession unsupported = VI_NULL;
    const auto unsupported_status =
        viOpen(rm, kInstrResource, VI_NO_LOCK, 1000, &unsupported);
    CHECK(unsupported_status ==
          (wrvisa::libusb_detail::provider_available()
               ? VI_ERROR_RSRC_NFOUND
               : VI_ERROR_NSUP_OPER));
    CHECK(viClose(rm) == VI_SUCCESS);
    return 0;
}
