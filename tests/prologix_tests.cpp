#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <future>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <asio.hpp>

#include "test_support.h"
#include "visa.h"
#include "webreal_visa_ext.h"

namespace {

struct InstrumentWrite {
    std::string address;
    std::vector<std::uint8_t> data;
    bool send_end{false};
};

class PrologixServer final {
public:
    PrologixServer()
        : acceptor_(context_, asio::ip::tcp::endpoint(
                                  asio::ip::address_v4::loopback(), 0)),
          client_(context_) {
        acceptor_.non_blocking(true);
        worker_ = std::thread([this] { run(); });
    }

    ~PrologixServer() {
        stopping_.store(true, std::memory_order_release);
        if (worker_.joinable()) {
            worker_.join();
        }
        asio::error_code ignored;
        client_.close(ignored);
        acceptor_.close(ignored);
    }

    std::uint16_t port() const { return acceptor_.local_endpoint().port(); }

    std::vector<InstrumentWrite> writes() const {
        std::lock_guard lock(mutex_);
        return writes_;
    }

    std::size_t connection_count() const {
        std::lock_guard lock(mutex_);
        return connections_;
    }

    std::size_t initialization_count() const {
        std::lock_guard lock(mutex_);
        return savecfg_zero_;
    }

    std::size_t read_command_count() const {
        std::lock_guard lock(mutex_);
        return read_commands_;
    }

    std::size_t clear_count() const {
        std::lock_guard lock(mutex_);
        return clears_;
    }

    std::size_t trigger_count() const {
        std::lock_guard lock(mutex_);
        return triggers_;
    }

    bool wait_for_reads(std::size_t expected) {
        std::unique_lock lock(mutex_);
        return condition_.wait_for(lock, std::chrono::seconds(2), [&] {
            return read_commands_ >= expected;
        });
    }

    bool wait_for_writes(std::size_t expected) {
        std::unique_lock lock(mutex_);
        return condition_.wait_for(lock, std::chrono::seconds(2), [&] {
            return writes_.size() >= expected;
        });
    }
    bool wait_for_clears(std::size_t expected) {
        std::unique_lock lock(mutex_);
        return condition_.wait_for(lock, std::chrono::seconds(2), [&] {
            return clears_ >= expected;
        });
    }

    bool wait_for_triggers(std::size_t expected) {
        std::unique_lock lock(mutex_);
        return condition_.wait_for(lock, std::chrono::seconds(2), [&] {
            return triggers_ >= expected;
        });
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

    void run() {
        while (!stopping_.load(std::memory_order_acquire)) {
            asio::error_code error;
            client_ = asio::ip::tcp::socket(context_);
            acceptor_.accept(client_, error);
            if (error == asio::error::would_block ||
                error == asio::error::try_again) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                continue;
            }
            if (error) {
                break;
            }
            {
                std::lock_guard lock(mutex_);
                ++connections_;
            }
            serve_connection();
            client_.close(error);
        }
    }

    void serve_connection() {
        std::vector<std::uint8_t> frame;
        bool escaped = false;
        std::array<std::uint8_t, 512> buffer{};
        asio::error_code error;
        while (!stopping_.load(std::memory_order_acquire)) {
            const auto amount = client_.read_some(asio::buffer(buffer), error);
            if (error) {
                return;
            }
            for (std::size_t index = 0; index < amount; ++index) {
                const auto value = buffer[index];
                if (escaped) {
                    frame.push_back(value);
                    escaped = false;
                    continue;
                }
                if (value == 0x1bu) {
                    frame.push_back(value);
                    escaped = true;
                    continue;
                }
                if (value == '\n') {
                    handle_frame(frame, error);
                    frame.clear();
                    if (error) {
                        return;
                    }
                    continue;
                }
                frame.push_back(value);
            }
        }
    }

    void handle_frame(const std::vector<std::uint8_t>& frame,
                      asio::error_code& error) {
        if (frame.size() >= 2 && frame[0] == '+' && frame[1] == '+') {
            handle_command(std::string(frame.begin(), frame.end()), error);
            return;
        }
        auto decoded = decode_data(frame);
        std::lock_guard lock(mutex_);
        writes_.push_back(InstrumentWrite{current_address_, decoded, send_end_});
        condition_.notify_all();
        if (decoded == std::vector<std::uint8_t>{'H', 'A', 'N', 'G', '?'}) {
            response_mode_ = ResponseMode::hang;
        } else if (decoded == std::vector<std::uint8_t>{'B', 'I', 'G', '?'}) {
            response_mode_ = ResponseMode::big;
        } else if (decoded == std::vector<std::uint8_t>{'*', 'I', 'D', 'N', '?'}) {
            response_mode_ = ResponseMode::idn;
        } else {
            response_mode_ = ResponseMode::echo;
            response_ = decoded;
        }
    }

    void handle_command(const std::string& command, asio::error_code& error) {
        if (command == "++savecfg 0") {
            std::lock_guard lock(mutex_);
            ++savecfg_zero_;
        } else if (command.rfind("++addr ", 0) == 0) {
            std::lock_guard lock(mutex_);
            current_address_ = command.substr(7);
        } else if (command == "++eoi 1" || command == "++eoi 0") {
            std::lock_guard lock(mutex_);
            send_end_ = command.back() == '1';
        } else if (command.rfind("++eot_char ", 0) == 0) {
            const auto value = std::stoul(command.substr(11));
            std::lock_guard lock(mutex_);
            eot_char_ = static_cast<std::uint8_t>(value);
        } else if (command == "++ver") {
            constexpr std::string_view version =
                "Prologix GPIB-ETHERNET Controller version 1.6.6.0\r\n";
            asio::write(client_, asio::buffer(version), error);
        } else if (command == "++read eoi") {
            std::vector<std::uint8_t> response;
            std::uint8_t marker = 0;
            ResponseMode mode = ResponseMode::echo;
            {
                std::lock_guard lock(mutex_);
                ++read_commands_;
                condition_.notify_all();
                mode = response_mode_;
                marker = eot_char_;
                response = response_;
            }
            if (mode == ResponseMode::hang) {
                return;
            }
            if (mode == ResponseMode::idn) {
                const std::string text = "WEBREAL,PROLOGIX-SIM,0001,0.6\nTAIL";
                response.assign(text.begin(), text.end());
            } else if (mode == ResponseMode::big) {
                response.assign(96u, 'X');
            }
            response.push_back(marker);
            asio::write(client_, asio::buffer(response), error);
        } else if (command.rfind("++spoll ", 0) == 0) {
            asio::write(client_, asio::buffer("64\r\n", 4), error);
        } else if (command == "++clr") {
            std::lock_guard lock(mutex_);
            ++clears_;
        } else if (command.rfind("++trg ", 0) == 0) {
            condition_.notify_all();
            std::lock_guard lock(mutex_);
            ++triggers_;
        }
            condition_.notify_all();
    }

    enum class ResponseMode {
        echo,
        idn,
        hang,
        big,
    };

    asio::io_context context_;
    asio::ip::tcp::acceptor acceptor_;
    asio::ip::tcp::socket client_;
    std::thread worker_;
    std::atomic<bool> stopping_{false};
    mutable std::mutex mutex_;
    std::condition_variable condition_;
    std::vector<InstrumentWrite> writes_;
    std::string current_address_;
    std::vector<std::uint8_t> response_;
    ResponseMode response_mode_{ResponseMode::echo};
    std::uint8_t eot_char_{0};
    bool send_end_{false};
    std::size_t connections_{0};
    std::size_t savecfg_zero_{0};
    std::size_t read_commands_{0};
    std::size_t clears_{0};
    std::size_t triggers_{0};
};

wrvisa_prologix_controller_config_v1 configuration(std::uint16_t port) {
    wrvisa_prologix_controller_config_v1 config{};
    config.struct_size = sizeof(config);
    config.abi_major = WRVISA_PROLOGIX_ABI_MAJOR;
    config.abi_minor = WRVISA_PROLOGIX_ABI_MINOR;
    config.connection_type = WRVISA_PROLOGIX_CONNECTION_TCP;
    config.tcp_port = port;
    config.eot_char = 0x04;
    config.read_timeout_ms = 100;
    config.maximum_response_size = 64;
    return config;
}

ViStatus write_bytes(ViSession session, const std::vector<std::uint8_t>& data) {
    ViUInt32 written = 0;
    const auto status = viWrite(session, data.data(),
                                static_cast<ViUInt32>(data.size()), &written);
    CHECK(status < VI_SUCCESS || written == data.size());
    return status;
}

std::string read_text(ViSession session, ViStatus expected) {
    std::array<ViByte, 256> data{};
    ViUInt32 read = 99;
    CHECK(viRead(session, data.data(), static_cast<ViUInt32>(data.size()), &read) ==
          expected);
    return {reinterpret_cast<const char*>(data.data()), read};
}

}  // namespace

int main() {
    ViSession validation_rm = VI_NULL;
    CHECK(viOpenDefaultRM(&validation_rm) == VI_SUCCESS);
    auto invalid = configuration(1234);
    CHECK(wrvisaSetPrologixController(VI_NULL, 7, "127.0.0.1", &invalid) ==
          VI_ERROR_INV_OBJECT);
    CHECK(wrvisaSetPrologixController(validation_rm, 7, nullptr, &invalid) ==
          VI_ERROR_INV_PARAMETER);
    CHECK(wrvisaSetPrologixController(validation_rm, 7, "127.0.0.1", nullptr) ==
          VI_ERROR_INV_PARAMETER);
    invalid.struct_size = sizeof(invalid) - 1u;
    CHECK(wrvisaSetPrologixController(validation_rm, 7, "127.0.0.1", &invalid) ==
          VI_ERROR_INV_SIZE);
    invalid = configuration(1234);
    invalid.abi_major = 99;
    CHECK(wrvisaSetPrologixController(validation_rm, 7, "127.0.0.1", &invalid) ==
          VI_ERROR_NSUP_OPER);
    invalid = configuration(1234);
    invalid.flags = 1;
    CHECK(wrvisaSetPrologixController(validation_rm, 7, "127.0.0.1", &invalid) ==
          VI_ERROR_INV_PARAMETER);
    invalid = configuration(0);
    CHECK(wrvisaSetPrologixController(validation_rm, 7, "127.0.0.1", &invalid) ==
          VI_ERROR_INV_PARAMETER);
    invalid = configuration(1234);
    invalid.read_timeout_ms = 3001;
    CHECK(wrvisaSetPrologixController(validation_rm, 7, "127.0.0.1", &invalid) ==
          VI_ERROR_INV_PARAMETER);
    invalid = configuration(1234);
    invalid.maximum_response_size = WRVISA_PROLOGIX_MAX_RESPONSE_SIZE + 1u;
    CHECK(wrvisaSetPrologixController(validation_rm, 7, "127.0.0.1", &invalid) ==
          VI_ERROR_INV_PARAMETER);
    CHECK(viClose(validation_rm) == VI_SUCCESS);

    PrologixServer server;
    auto config = configuration(server.port());
    ViSession rm = VI_NULL;
    CHECK(viOpenDefaultRM(&rm) == VI_SUCCESS);
    CHECK(wrvisaSetPrologixController(rm, 7, "127.0.0.1", &config) ==
          VI_SUCCESS);
    CHECK(wrvisaSetPrologixController(rm, 8, "127.0.0.1", &config) ==
          VI_SUCCESS);

    ViSession conflicting_rm = VI_NULL;
    CHECK(viOpenDefaultRM(&conflicting_rm) == VI_SUCCESS);
    auto conflicting = config;
    conflicting.eot_char = 0x03;
    CHECK(wrvisaSetPrologixController(conflicting_rm, 9, "127.0.0.1",
                                     &conflicting) == VI_ERROR_RSRC_BUSY);
    CHECK(viClose(conflicting_rm) == VI_SUCCESS);

    std::array<ViChar, VI_FIND_BUFLEN> found{};
    CHECK(viFindRsrc(rm, "GPIB7?*", nullptr, nullptr, found.data()) ==
          VI_ERROR_RSRC_NFOUND);

    ViSession first = VI_NULL;
    ViSession second = VI_NULL;
    CHECK(viOpen(rm, "GPIB7::4::2::INSTR", VI_NO_LOCK, 1000, &first) ==
          VI_SUCCESS);
    CHECK(viOpen(rm, "GPIB8::5::INSTR", VI_NO_LOCK, 1000, &second) ==
          VI_SUCCESS);
    CHECK(server.connection_count() == 1);
    CHECK(server.initialization_count() == 1);

    const std::vector<std::uint8_t> binary{'A', '\r', '\n', 0x1b, '+', 'Z'};
    CHECK(write_bytes(first, binary) == VI_SUCCESS);
    CHECK(server.wait_for_writes(1));
    auto writes = server.writes();
    CHECK(!writes.empty());
    CHECK(writes.back().address == "4 98");
    CHECK(writes.back().data == binary);
    CHECK(writes.back().send_end);

    CHECK(viSetAttribute(first, VI_ATTR_SEND_END_EN, VI_FALSE) == VI_SUCCESS);
    CHECK(write_bytes(first, std::vector<std::uint8_t>{'N', 'O', 'E', 'O', 'I'}) ==
          VI_SUCCESS);
    CHECK(server.wait_for_writes(2));
    writes = server.writes();
    CHECK(!writes.back().send_end);
    CHECK(viSetAttribute(first, VI_ATTR_SEND_END_EN, VI_TRUE) == VI_SUCCESS);

    CHECK(write_bytes(first, std::vector<std::uint8_t>{'*', 'I', 'D', 'N', '?'}) ==
          VI_SUCCESS);
    CHECK(viSetAttribute(first, VI_ATTR_TERMCHAR, '\n') == VI_SUCCESS);
    CHECK(viSetAttribute(first, VI_ATTR_TERMCHAR_EN, VI_TRUE) == VI_SUCCESS);
    CHECK(read_text(first, VI_SUCCESS_TERM_CHAR) ==
          "WEBREAL,PROLOGIX-SIM,0001,0.6\n");
    CHECK(viSetAttribute(first, VI_ATTR_TERMCHAR_EN, VI_FALSE) == VI_SUCCESS);
    CHECK(read_text(first, VI_SUCCESS) == "TAIL");

    CHECK(write_bytes(first, std::vector<std::uint8_t>{'*', 'I', 'D', 'N', '?'}) ==
          VI_SUCCESS);
    std::array<ViByte, 4> prefix{};
    ViUInt32 prefix_size = 99;
    CHECK(viRead(first, prefix.data(), static_cast<ViUInt32>(prefix.size()),
                 &prefix_size) == VI_SUCCESS_MAX_CNT);
    CHECK(prefix_size == prefix.size());
    CHECK(std::string(reinterpret_cast<const char*>(prefix.data()),
                      prefix_size) == "WEBR");
    CHECK(viFlush(first, VI_READ_BUF_DISCARD) == VI_SUCCESS);
    CHECK(write_bytes(first, std::vector<std::uint8_t>{'*', 'I', 'D', 'N', '?'}) ==
          VI_SUCCESS);
    CHECK(viSetAttribute(first, VI_ATTR_TERMCHAR_EN, VI_TRUE) == VI_SUCCESS);
    CHECK(read_text(first, VI_SUCCESS_TERM_CHAR) ==
          "WEBREAL,PROLOGIX-SIM,0001,0.6\n");
    CHECK(viSetAttribute(first, VI_ATTR_TERMCHAR_EN, VI_FALSE) == VI_SUCCESS);
    CHECK(read_text(first, VI_SUCCESS) == "TAIL");

    ViUInt16 status_byte = 0;
    CHECK(viReadSTB(first, &status_byte) == VI_SUCCESS);
    CHECK(status_byte == 64);
    CHECK(viAssertTrigger(first, VI_TRIG_PROT_DEFAULT) == VI_SUCCESS);
    CHECK(viClear(first) == VI_SUCCESS);
    CHECK(server.wait_for_triggers(1));
    CHECK(server.wait_for_clears(1));

    const auto writes_before_concurrent = server.writes().size();
    auto concurrent_first = std::async(std::launch::async, [&] {
        return write_bytes(first, std::vector<std::uint8_t>{'F', 'I', 'R', 'S', 'T'});
    });
    auto concurrent_second = std::async(std::launch::async, [&] {
        return write_bytes(second,
                           std::vector<std::uint8_t>{'S', 'E', 'C', 'O', 'N', 'D'});
    });
    CHECK(concurrent_first.get() == VI_SUCCESS);
    CHECK(concurrent_second.get() == VI_SUCCESS);
    CHECK(server.wait_for_writes(writes_before_concurrent + 2u));
    writes = server.writes();
    CHECK(std::any_of(writes.begin(), writes.end(), [](const InstrumentWrite& write) {
        return write.address == "4 98" &&
               write.data == std::vector<std::uint8_t>{'F', 'I', 'R', 'S', 'T'};
    }));
    CHECK(std::any_of(writes.begin(), writes.end(), [](const InstrumentWrite& write) {
        return write.address == "5" &&
               write.data ==
                   std::vector<std::uint8_t>{'S', 'E', 'C', 'O', 'N', 'D'};
    }));

    CHECK(write_bytes(first, std::vector<std::uint8_t>{'B', 'I', 'G', '?'}) ==
          VI_SUCCESS);
    CHECK(viSetAttribute(first, VI_ATTR_TMO_VALUE, 1000) == VI_SUCCESS);
    std::array<ViByte, 128> buffer{};
    ViUInt32 read = 99;
    CHECK(viRead(first, buffer.data(), static_cast<ViUInt32>(buffer.size()), &read) ==
          VI_ERROR_IO);
    CHECK(read == 0);

    CHECK(write_bytes(first, std::vector<std::uint8_t>{'*', 'I', 'D', 'N', '?'}) ==
          VI_SUCCESS);
    CHECK(server.connection_count() >= 2);
    CHECK(server.initialization_count() >= 2);
    CHECK(viSetAttribute(first, VI_ATTR_TERMCHAR_EN, VI_TRUE) == VI_SUCCESS);
    CHECK(read_text(first, VI_SUCCESS_TERM_CHAR) ==
          "WEBREAL,PROLOGIX-SIM,0001,0.6\n");
    CHECK(viSetAttribute(first, VI_ATTR_TERMCHAR_EN, VI_FALSE) == VI_SUCCESS);
    CHECK(read_text(first, VI_SUCCESS) == "TAIL");

    CHECK(write_bytes(first, std::vector<std::uint8_t>{'H', 'A', 'N', 'G', '?'}) ==
          VI_SUCCESS);
    CHECK(viSetAttribute(first, VI_ATTR_TERMCHAR_EN, VI_FALSE) == VI_SUCCESS);
    CHECK(viSetAttribute(first, VI_ATTR_TMO_VALUE, VI_TMO_INFINITE) == VI_SUCCESS);
    const auto initializations_before_cancel = server.initialization_count();
    const auto reads_before_cancel = server.read_command_count();
    auto cancelled = std::async(std::launch::async, [&] {
        ViUInt32 local_read = 99;
        const auto status = viRead(first, buffer.data(), 1, &local_read);
        return std::pair{status, local_read};
    });
    CHECK(server.wait_for_reads(reads_before_cancel + 1u));
    CHECK(viSetAttribute(second, VI_ATTR_TMO_VALUE, 30) == VI_SUCCESS);
    const std::vector<std::uint8_t> queued{'Q', 'U', 'E', 'U', 'E', 'D'};
    ViUInt32 queued_written = 99;
    CHECK(viWrite(second, queued.data(), static_cast<ViUInt32>(queued.size()),
                  &queued_written) == VI_ERROR_TMO);
    CHECK(queued_written == 0);
    CHECK(viSetAttribute(second, VI_ATTR_TMO_VALUE, 1000) == VI_SUCCESS);

    CHECK(viTerminate(first, VI_NULL, VI_NULL) == VI_SUCCESS);
    const auto cancelled_result = cancelled.get();
    CHECK(cancelled_result.first == VI_ERROR_ABORT);
    CHECK(cancelled_result.second == 0);

    CHECK(write_bytes(first, std::vector<std::uint8_t>{'*', 'I', 'D', 'N', '?'}) ==
          VI_SUCCESS);
    CHECK(server.initialization_count() > initializations_before_cancel);

    CHECK(viClose(second) == VI_SUCCESS);
    CHECK(viClose(first) == VI_SUCCESS);
    CHECK(viClose(rm) == VI_SUCCESS);
    return 0;
}
