#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <cerrno>
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>

#include "test_support.h"
#include "visa.h"
#include "webreal_visa_ext.h"

namespace {

struct InstrumentWrite {
    std::string address;
    std::vector<std::uint8_t> data;
    bool send_end{false};
};

class PrologixSerialController final {
public:
    PrologixSerialController() {
        master_ = posix_openpt(O_RDWR | O_NOCTTY);
        CHECK(master_ >= 0);
        CHECK(grantpt(master_) == 0);
        CHECK(unlockpt(master_) == 0);
        const char* path = ptsname(master_);
        CHECK(path != nullptr);
        path_ = path;
        worker_ = std::thread([this] { run(); });
    }

    ~PrologixSerialController() {
        stopping_.store(true, std::memory_order_release);
        if (worker_.joinable()) {
            worker_.join();
        }
        CHECK(close(master_) == 0);
    }

    const std::string& path() const noexcept { return path_; }

    bool wait_for_initialization() {
        std::unique_lock lock(mutex_);
        return condition_.wait_for(lock, std::chrono::seconds(2), [&] {
            return savecfg_zero_ != 0;
        });
    }

    bool wait_for_writes(std::size_t expected) {
        std::unique_lock lock(mutex_);
        return condition_.wait_for(lock, std::chrono::seconds(2), [&] {
            return writes_.size() >= expected;
        });
    }

    std::vector<InstrumentWrite> writes() const {
        std::lock_guard lock(mutex_);
        return writes_;
    }

private:
    static std::vector<std::uint8_t> decode_data(
        const std::vector<std::uint8_t>& frame) {
        std::vector<std::uint8_t> decoded;
        decoded.reserve(frame.size());
        bool escaped = false;
        for (const auto value : frame) {
            if (escaped) {
                decoded.push_back(value);
                escaped = false;
            } else if (value == 0x1bu) {
                escaped = true;
            } else {
                decoded.push_back(value);
            }
        }
        if (escaped) {
            decoded.push_back(0x1bu);
        }
        return decoded;
    }

    bool write_all(const std::vector<std::uint8_t>& data) {
        std::size_t offset = 0;
        while (offset < data.size()) {
            const auto amount =
                write(master_, data.data() + offset, data.size() - offset);
            if (amount > 0) {
                offset += static_cast<std::size_t>(amount);
            } else if (amount < 0 && errno == EINTR) {
                continue;
            } else {
                return false;
            }
        }
        return true;
    }

    bool write_text(const std::string& text) {
        return write_all(
            std::vector<std::uint8_t>(text.begin(), text.end()));
    }

    void run() {
        std::vector<std::uint8_t> frame;
        bool escaped = false;
        std::array<std::uint8_t, 512> buffer{};
        while (!stopping_.load(std::memory_order_acquire)) {
            pollfd event{master_, POLLIN, 0};
            const auto ready = poll(&event, 1, 50);
            if (ready < 0) {
                if (errno == EINTR) {
                    continue;
                }
                return;
            }
            if (ready == 0 || (event.revents & POLLIN) == 0) {
                continue;
            }
            const auto amount = read(master_, buffer.data(), buffer.size());
            if (amount <= 0) {
                if (amount < 0 && errno == EINTR) {
                    continue;
                }
                return;
            }
            for (std::size_t index = 0;
                 index < static_cast<std::size_t>(amount); ++index) {
                const auto value = buffer[index];
                if (escaped) {
                    frame.push_back(value);
                    escaped = false;
                } else if (value == 0x1bu) {
                    frame.push_back(value);
                    escaped = true;
                } else if (value == '\n') {
                    if (!handle_frame(frame)) {
                        return;
                    }
                    frame.clear();
                } else {
                    frame.push_back(value);
                }
            }
        }
    }

    bool handle_frame(const std::vector<std::uint8_t>& frame) {
        if (frame.size() >= 2 && frame[0] == '+' && frame[1] == '+') {
            return handle_command(std::string(frame.begin(), frame.end()));
        }
        auto decoded = decode_data(frame);
        std::lock_guard lock(mutex_);
        writes_.push_back(InstrumentWrite{address_, std::move(decoded), send_end_});
        condition_.notify_all();
        return true;
    }

    bool handle_command(const std::string& command) {
        if (command == "++savecfg 0") {
            std::lock_guard lock(mutex_);
            ++savecfg_zero_;
            condition_.notify_all();
        } else if (command.rfind("++addr ", 0) == 0) {
            std::lock_guard lock(mutex_);
            address_ = command.substr(7);
        } else if (command == "++eoi 1" || command == "++eoi 0") {
            std::lock_guard lock(mutex_);
            send_end_ = command.back() == '1';
        } else if (command.rfind("++eot_char ", 0) == 0) {
            const auto value = std::stoul(command.substr(11));
            std::lock_guard lock(mutex_);
            eot_char_ = static_cast<std::uint8_t>(value);
        } else if (command == "++ver") {
            return write_text("Prologix GPIB-USB Controller version 6.107\r\n");
        } else if (command == "++read eoi") {
            std::uint8_t marker = 0;
            {
                std::lock_guard lock(mutex_);
                marker = eot_char_;
            }
            std::vector<std::uint8_t> response{
                'S', 'E', 'R', 'I', 'A', 'L', '-', 'O', 'K', '\n', marker};
            return write_all(response);
        }
        return true;
    }

    int master_{-1};
    std::string path_;
    std::thread worker_;
    std::atomic<bool> stopping_{false};
    mutable std::mutex mutex_;
    std::condition_variable condition_;
    std::string address_;
    std::vector<InstrumentWrite> writes_;
    std::uint8_t eot_char_{0};
    bool send_end_{false};
    std::size_t savecfg_zero_{0};
};

wrvisa_prologix_controller_config_v1 serial_configuration() {
    wrvisa_prologix_controller_config_v1 config{};
    config.struct_size = sizeof(config);
    config.abi_major = WRVISA_PROLOGIX_ABI_MAJOR;
    config.abi_minor = WRVISA_PROLOGIX_ABI_MINOR;
    config.connection_type = WRVISA_PROLOGIX_CONNECTION_SERIAL;
    config.eot_char = 0x04;
    config.read_timeout_ms = 100;
    config.maximum_response_size = 1024;
    return config;
}

}  // namespace

int main() {
    PrologixSerialController controller;
    ViSession rm = VI_NULL;
    CHECK(viOpenDefaultRM(&rm) == VI_SUCCESS);

    auto invalid = serial_configuration();
    invalid.tcp_port = 1234;
    CHECK(wrvisaSetPrologixController(rm, 9, controller.path().c_str(), &invalid) ==
          VI_ERROR_INV_PARAMETER);

    const auto config = serial_configuration();
    CHECK(wrvisaSetPrologixController(rm, 9, controller.path().c_str(), &config) ==
          VI_SUCCESS);
    CHECK(wrvisaSetPrologixController(rm, 10, "/wrvisa/does/not/exist",
                                      &config) == VI_SUCCESS);

    ViSession missing = VI_NULL;
    CHECK(viOpen(rm, "GPIB10::6::INSTR", VI_NO_LOCK, 1000, &missing) ==
          VI_ERROR_RSRC_NFOUND);

    ViSession session = VI_NULL;
    CHECK(viOpen(rm, "GPIB9::6::INSTR", VI_NO_LOCK, 1000, &session) ==
          VI_SUCCESS);
    CHECK(controller.wait_for_initialization());

    const std::vector<std::uint8_t> request{
        'P', 'I', 'N', 'G', '+', '\r', '\n', 0x1b, '?'};
    ViUInt32 transferred = 0;
    CHECK(viWrite(session, request.data(), static_cast<ViUInt32>(request.size()),
                  &transferred) == VI_SUCCESS);
    CHECK(transferred == request.size());
    CHECK(controller.wait_for_writes(1));
    const auto writes = controller.writes();
    CHECK(writes.back().address == "6");
    CHECK(writes.back().data == request);
    CHECK(writes.back().send_end);

    std::array<ViByte, 64> response{};
    transferred = 99;
    CHECK(viRead(session, response.data(), static_cast<ViUInt32>(response.size()),
                 &transferred) == VI_SUCCESS);
    CHECK(std::string(reinterpret_cast<const char*>(response.data()), transferred) ==
          "SERIAL-OK\n");

    CHECK(viClose(session) == VI_SUCCESS);
    CHECK(viClose(rm) == VI_SUCCESS);
    return 0;
}
