#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <future>
#include <memory>
#include <mutex>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "backends/gpib/gpib_provider.h"
#include "test_support.h"
#include "visa.h"

namespace {

constexpr const char* kResource = "GPIB7::4::2::INSTR";
constexpr const char* kLimitedResource = "GPIB7::5::INSTR";

struct ControllerState {
    std::mutex mutex;
    std::condition_variable condition;
    bool connected{true};
    std::size_t reads_waiting{0};
    std::size_t opens{0};
    std::size_t closes{0};
    std::size_t clears{0};
    std::size_t triggers{0};
    std::size_t serial_polls{0};
    std::vector<bool> send_end_values;
};

class FakeGpibTransport final : public wrvisa::GpibTransport {
public:
    FakeGpibTransport(std::shared_ptr<ControllerState> state,
                      wrvisa::GpibCapabilities capabilities)
        : state_(std::move(state)), capabilities_(capabilities) {}

    wrvisa::GpibCapabilities capabilities() const noexcept override {
        return capabilities_;
    }

    ViStatus read(wrvisa::Operation& operation, std::size_t maximum_size,
                  std::vector<std::uint8_t>& data, bool& end) override {
        std::unique_lock lock(state_->mutex);
        struct WaitingRead {
            explicit WaitingRead(ControllerState& value) : state(value) {
                ++state.reads_waiting;
                state.condition.notify_all();
            }
            ~WaitingRead() { --state.reads_waiting; }
            ControllerState& state;
        } waiting(*state_);
        const auto ready = [&] {
            return !response_.empty() || closed_ || !state_->connected ||
                   operation.completed();
        };
        if (operation.deadline() == wrvisa::Operation::Clock::time_point::max()) {
            state_->condition.wait(lock, ready);
        } else if (!state_->condition.wait_until(lock, operation.deadline(),
                                                 ready)) {
            static_cast<void>(operation.request_timeout());
        }
        if (operation.completed()) {
            return operation.result();
        }
        if (closed_ || !state_->connected) {
            return VI_ERROR_CONN_LOST;
        }
        const auto amount = std::min(maximum_size, response_.size());
        data.assign(response_.begin(), response_.begin() +
                                           static_cast<std::ptrdiff_t>(amount));
        response_.erase(0, amount);
        end = response_.empty();
        return VI_SUCCESS;
    }

    ViStatus write(wrvisa::Operation& operation,
                   std::span<const std::uint8_t> data, bool send_end,
                   std::size_t& transferred) override {
        transferred = 0;
        if (operation.completed()) {
            return operation.result();
        }
        std::lock_guard lock(state_->mutex);
        if (closed_ || !state_->connected) {
            return VI_ERROR_CONN_LOST;
        }
        state_->send_end_values.push_back(send_end);
        std::string request(data.begin(), data.end());
        response_ = request == "*IDN?\n"
                        ? "WEBREAL,GPIB-SIM,0001,0.6\nTAIL"
                        : request;
        transferred = data.size();
        state_->condition.notify_all();
        return VI_SUCCESS;
    }

    ViStatus clear(wrvisa::Operation& operation) override {
        if (operation.completed()) {
            return operation.result();
        }
        std::lock_guard lock(state_->mutex);
        if (closed_ || !state_->connected) {
            return VI_ERROR_CONN_LOST;
        }
        response_.clear();
        ++state_->clears;
        return VI_SUCCESS;
    }

    void discard_read_buffer() noexcept override {
        std::lock_guard lock(state_->mutex);
        response_.clear();
    }

    ViStatus trigger(wrvisa::Operation& operation) override {
        if (operation.completed()) {
            return operation.result();
        }
        std::lock_guard lock(state_->mutex);
        if (closed_ || !state_->connected) {
            return VI_ERROR_CONN_LOST;
        }
        ++state_->triggers;
        return VI_SUCCESS;
    }

    ViStatus serial_poll(wrvisa::Operation& operation,
                         std::uint8_t& status_byte) override {
        if (operation.completed()) {
            return operation.result();
        }
        std::lock_guard lock(state_->mutex);
        if (closed_ || !state_->connected) {
            return VI_ERROR_CONN_LOST;
        }
        ++state_->serial_polls;
        status_byte = 0x40;
        return VI_SUCCESS;
    }

    void cancel() noexcept override { state_->condition.notify_all(); }

    void close() noexcept override {
        {
            std::lock_guard lock(state_->mutex);
            if (!closed_) {
                closed_ = true;
                ++state_->closes;
            }
        }
        state_->condition.notify_all();
    }

private:
    std::shared_ptr<ControllerState> state_;
    wrvisa::GpibCapabilities capabilities_;
    std::string response_;
    bool closed_{false};
};

class FakeGpibProvider final : public wrvisa::GpibProvider {
public:
    explicit FakeGpibProvider(std::shared_ptr<ControllerState> state)
        : state_(std::move(state)) {}

    std::vector<std::string> discover() override {
        return {"gpib7::4::2", "GPIB7::INTFC", "GPIB7::4::2::INSTR",
                "not-a-resource"};
    }

    std::unique_ptr<wrvisa::GpibTransport> open(
        const wrvisa::ResourceDescriptor& resource, ViUInt32 timeout,
        ViStatus& status) override {
        static_cast<void>(timeout);
        if (resource.kind != wrvisa::ResourceKind::gpib_instr ||
            resource.interface_number != 7) {
            status = VI_ERROR_RSRC_NFOUND;
            return nullptr;
        }
        wrvisa::GpibCapabilities capabilities;
        if (resource.gpib_primary_address == 4 &&
            resource.gpib_has_secondary_address &&
            resource.gpib_secondary_address == 2) {
            capabilities = {true, true, true, true};
        } else if (resource.gpib_primary_address != 5 ||
                   resource.gpib_has_secondary_address) {
            status = VI_ERROR_RSRC_NFOUND;
            return nullptr;
        }
        {
            std::lock_guard lock(state_->mutex);
            ++state_->opens;
        }
        status = VI_SUCCESS;
        return std::make_unique<FakeGpibTransport>(state_, capabilities);
    }

private:
    std::shared_ptr<ControllerState> state_;
};

class DiagnosticProvider final : public wrvisa::GpibProvider {
public:
    std::vector<std::string> discover() override {
        throw std::runtime_error("isolated discovery failure");
    }

    std::unique_ptr<wrvisa::GpibTransport> open(
        const wrvisa::ResourceDescriptor& resource, ViUInt32 timeout,
        ViStatus& status) override {
        static_cast<void>(resource);
        static_cast<void>(timeout);
        status = VI_ERROR_IO;
        return nullptr;
    }
};

bool wait_for_read(const std::shared_ptr<ControllerState>& state) {
    std::unique_lock lock(state->mutex);
    return state->condition.wait_for(lock, std::chrono::seconds(2), [&] {
        return state->reads_waiting != 0;
    });
}

}  // namespace

int main() {
    CHECK(!wrvisa::register_gpib_provider(nullptr));

    ViSession empty_rm = VI_NULL;
    CHECK(viOpenDefaultRM(&empty_rm) == VI_SUCCESS);
    std::array<ViChar, VI_FIND_BUFLEN> found{};
    CHECK(viFindRsrc(empty_rm, "GPIB?*", nullptr, nullptr, found.data()) ==
          VI_ERROR_RSRC_NFOUND);

    auto state = std::make_shared<ControllerState>();
    auto diagnostic = wrvisa::register_gpib_provider(
        std::make_shared<DiagnosticProvider>());
    auto registration = wrvisa::register_gpib_provider(
        std::make_shared<FakeGpibProvider>(state));
    CHECK(diagnostic);
    CHECK(registration);

    ViSession rm = VI_NULL;
    CHECK(viOpenDefaultRM(&rm) == VI_SUCCESS);
    ViFindList list = VI_NULL;
    ViUInt32 count = 0;
    CHECK(viFindRsrc(rm, "GPIB7?*", &list, &count, found.data()) == VI_SUCCESS);
    CHECK(count == 2);
    CHECK(std::string(found.data()) == kResource);
    CHECK(viFindNext(list, found.data()) == VI_SUCCESS);
    CHECK(std::string(found.data()) == "GPIB7::INTFC");
    CHECK(viFindNext(list, found.data()) == VI_ERROR_RSRC_NFOUND);
    CHECK(viClose(list) == VI_SUCCESS);

    ViSession session = VI_NULL;
    CHECK(viOpen(rm, "GPIB7::INTFC", VI_NO_LOCK, 1000, &session) ==
          VI_ERROR_NSUP_OPER);
    CHECK(viOpen(rm, "GPIB7::6::INSTR", VI_NO_LOCK, 1000, &session) ==
          VI_ERROR_IO);
    CHECK(viOpen(rm, kResource, VI_NO_LOCK, 1000, &session) == VI_SUCCESS);

    ViUInt16 interface_type = 0;
    ViUInt16 interface_number = 0;
    CHECK(viGetAttribute(session, VI_ATTR_INTF_TYPE, &interface_type) == VI_SUCCESS);
    CHECK(viGetAttribute(session, VI_ATTR_INTF_NUM, &interface_number) == VI_SUCCESS);
    CHECK(interface_type == VI_INTF_GPIB);
    CHECK(interface_number == 7);
    std::array<ViChar, VI_FIND_BUFLEN> name{};
    CHECK(viGetAttribute(session, VI_ATTR_RSRC_NAME, name.data()) == VI_SUCCESS);
    CHECK(std::string(name.data()) == kResource);

    const std::string query = "*IDN?\n";
    ViUInt32 transferred = 0;
    CHECK(viWrite(session, reinterpret_cast<ViConstBuf>(query.data()),
                  static_cast<ViUInt32>(query.size()), &transferred) == VI_SUCCESS);
    CHECK(transferred == query.size());
    {
        std::lock_guard lock(state->mutex);
        CHECK(!state->send_end_values.empty());
        CHECK(state->send_end_values.back());
    }

    CHECK(viSetAttribute(session, VI_ATTR_TERMCHAR, '\n') == VI_SUCCESS);
    CHECK(viSetAttribute(session, VI_ATTR_TERMCHAR_EN, VI_TRUE) == VI_SUCCESS);
    std::array<ViByte, 128> buffer{};
    ViUInt32 read = 0;
    CHECK(viRead(session, buffer.data(), static_cast<ViUInt32>(buffer.size()),
                 &read) == VI_SUCCESS_TERM_CHAR);
    CHECK(std::string(reinterpret_cast<const char*>(buffer.data()), read) ==
          "WEBREAL,GPIB-SIM,0001,0.6\n");
    CHECK(viSetAttribute(session, VI_ATTR_TERMCHAR_EN, VI_FALSE) == VI_SUCCESS);
    CHECK(viRead(session, buffer.data(), static_cast<ViUInt32>(buffer.size()),
                 &read) == VI_SUCCESS);
    CHECK(std::string(reinterpret_cast<const char*>(buffer.data()), read) == "TAIL");

    ViUInt16 status_byte = 0;
    CHECK(viReadSTB(session, &status_byte) == VI_SUCCESS);
    CHECK(status_byte == 0x40);
    CHECK(viAssertTrigger(session, 99) == VI_ERROR_INV_PROT);
    CHECK(viAssertTrigger(session, VI_TRIG_PROT_DEFAULT) == VI_SUCCESS);
    CHECK(viClear(session) == VI_SUCCESS);
    CHECK(viFlush(session, VI_READ_BUF_DISCARD) == VI_SUCCESS);
    {
        std::lock_guard lock(state->mutex);
        CHECK(state->serial_polls == 1);
        CHECK(state->triggers == 1);
        CHECK(state->clears == 1);
    }

    CHECK(viSetAttribute(session, VI_ATTR_TMO_VALUE, VI_TMO_INFINITE) ==
          VI_SUCCESS);
    auto cancelled = std::async(std::launch::async, [&] {
        ViUInt32 local_read = 99;
        const auto status = viRead(session, buffer.data(), 1, &local_read);
        return std::pair{status, local_read};
    });
    CHECK(wait_for_read(state));
    CHECK(viTerminate(session, VI_NULL, VI_NULL) == VI_SUCCESS);
    const auto cancelled_result = cancelled.get();
    CHECK(cancelled_result.first == VI_ERROR_ABORT);
    CHECK(cancelled_result.second == 0);

    CHECK(viSetAttribute(session, VI_ATTR_TMO_VALUE, 25) == VI_SUCCESS);
    read = 99;
    CHECK(viRead(session, buffer.data(), 1, &read) == VI_ERROR_TMO);
    CHECK(read == 0);

    CHECK(viWrite(session, reinterpret_cast<ViConstBuf>(query.data()),
                  static_cast<ViUInt32>(query.size()), &transferred) == VI_SUCCESS);
    CHECK(viSetAttribute(session, VI_ATTR_TERMCHAR_EN, VI_TRUE) == VI_SUCCESS);
    CHECK(viRead(session, buffer.data(), static_cast<ViUInt32>(buffer.size()),
                 &read) == VI_SUCCESS_TERM_CHAR);

    ViSession limited = VI_NULL;
    CHECK(viOpen(rm, kLimitedResource, VI_NO_LOCK, 1000, &limited) == VI_SUCCESS);
    CHECK(viWrite(limited, reinterpret_cast<ViConstBuf>(query.data()),
                  static_cast<ViUInt32>(query.size()), &transferred) ==
          VI_ERROR_NSUP_OPER);
    CHECK(viSetAttribute(limited, VI_ATTR_SEND_END_EN, VI_FALSE) == VI_SUCCESS);
    CHECK(viWrite(limited, reinterpret_cast<ViConstBuf>(query.data()),
                  static_cast<ViUInt32>(query.size()), &transferred) == VI_SUCCESS);
    CHECK(viClear(limited) == VI_ERROR_NSUP_OPER);
    CHECK(viReadSTB(limited, &status_byte) == VI_ERROR_NSUP_OPER);
    CHECK(viAssertTrigger(limited, VI_TRIG_PROT_DEFAULT) == VI_ERROR_NSUP_OPER);
    CHECK(viClose(limited) == VI_SUCCESS);

    CHECK(viClose(session) == VI_SUCCESS);
    diagnostic.reset();
    registration.reset();
    CHECK(viOpen(rm, kResource, VI_NO_LOCK, 1000, &session) ==
          VI_ERROR_NSUP_OPER);
    CHECK(viFindRsrc(rm, "GPIB7?*", nullptr, &count, found.data()) == VI_SUCCESS);
    CHECK(count == 2);

    {
        std::lock_guard lock(state->mutex);
        CHECK(state->opens == 2);
        CHECK(state->closes == 2);
    }
    CHECK(viClose(rm) == VI_SUCCESS);
    CHECK(viClose(empty_rm) == VI_SUCCESS);
    return 0;
}
