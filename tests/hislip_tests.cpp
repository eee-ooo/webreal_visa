#include <array>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <future>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <asio.hpp>

#include "backends/hislip/hislip_protocol.h"
#include "test_support.h"
#include "visa.h"
#include "webreal_visa_ext.h"

namespace {

using wrvisa::hislip::Frame;
using wrvisa::hislip::MessageType;

Frame read_frame(asio::ip::tcp::socket& socket) {
    std::array<std::uint8_t, 16> header{};
    asio::error_code error;
    asio::read(socket, asio::buffer(header), error);
    CHECK(!error);
    std::uint64_t size = 0;
    for (std::size_t index = 8; index < header.size(); ++index) {
        size = (size << 8u) | header[index];
    }
    CHECK(size <= 16u * 1024u * 1024u);
    std::vector<std::uint8_t> bytes(header.begin(), header.end());
    bytes.resize(16u + static_cast<std::size_t>(size));
    if (size != 0) {
        asio::read(socket,
                   asio::buffer(bytes.data() + 16u, static_cast<std::size_t>(size)),
                   error);
        CHECK(!error);
    }
    Frame frame;
    CHECK(wrvisa::hislip::decode(bytes, frame));
    return frame;
}

void write_frame(asio::ip::tcp::socket& socket, MessageType type,
                 std::uint8_t control, std::uint32_t parameter,
                 std::string payload = {}) {
    const auto bytes = wrvisa::hislip::encode(
        type, control, parameter,
        std::span<const std::uint8_t>(
            reinterpret_cast<const std::uint8_t*>(payload.data()), payload.size()));
    asio::error_code error;
    asio::write(socket, asio::buffer(bytes), error);
    CHECK(!error);
}

class HiSlipSimulator final {
public:
    HiSlipSimulator()
        : acceptor_(context_, asio::ip::tcp::endpoint(
                                  asio::ip::address_v4::loopback(), 0)) {
        worker_ = std::thread([this] { run(); });
    }

    ~HiSlipSimulator() {
        asio::error_code ignored;
        acceptor_.close(ignored);
        synchronous_.close(ignored);
        asynchronous_.close(ignored);
        if (worker_.joinable()) {
            worker_.join();
        }
    }

    std::uint16_t port() const { return acceptor_.local_endpoint().port(); }
    bool triggered() const noexcept { return triggered_; }
    bool wait_for_timeout_request() {
        std::unique_lock lock(timeout_mutex_);
        return timeout_condition_.wait_for(
            lock, std::chrono::seconds(2),
            [this] { return timeout_request_received_; });
    }

private:
    void run() {
        asio::error_code error;
        acceptor_.accept(synchronous_, error);
        CHECK(!error);
        auto frame = read_frame(synchronous_);
        CHECK(frame.type == MessageType::initialize);
        CHECK(frame.parameter >> 16u == UINT16_C(0x0100));
        CHECK(std::string(frame.payload.begin(), frame.payload.end()) == "hislip0");
        write_frame(synchronous_, MessageType::initialize_response, 0,
                    (UINT32_C(0x0100) << 16u) | UINT32_C(0x1234));

        acceptor_.accept(asynchronous_, error);
        CHECK(!error);
        frame = read_frame(asynchronous_);
        CHECK(frame.type == MessageType::async_initialize);
        CHECK(frame.parameter == UINT32_C(0x1234));
        write_frame(asynchronous_, MessageType::async_initialize_response, 0,
                    UINT32_C(0x4B53));

        frame = read_frame(asynchronous_);
        CHECK(frame.type == MessageType::async_maximum_message_size);
        CHECK(frame.payload.size() == 8u);
        const std::string maximum("\0\0\0\0\0\0\x10\0", 8);
        write_frame(asynchronous_, MessageType::async_maximum_message_size_response,
                    0, 0, maximum);

        frame = read_frame(synchronous_);
        CHECK(frame.type == MessageType::data_end);
        CHECK(std::string(frame.payload.begin(), frame.payload.end()) == "*IDN?\n");
        write_frame(synchronous_, MessageType::data_end, 0, frame.parameter,
                    "HISLIP-OK\n");

        frame = read_frame(synchronous_);
        CHECK(frame.type == MessageType::data_end);
        CHECK(std::string(frame.payload.begin(), frame.payload.end()) == "SPLIT\n");
        write_frame(synchronous_, MessageType::data, 0, frame.parameter, "AB");
        write_frame(synchronous_, MessageType::data_end, 0, frame.parameter, "CD\n");

        frame = read_frame(asynchronous_);
        CHECK(frame.type == MessageType::async_status_query);
        write_frame(asynchronous_, MessageType::async_service_request, 0x33, 0);
        write_frame(asynchronous_, MessageType::async_status_response, 0x42, 0);

        frame = read_frame(synchronous_);
        CHECK(frame.type == MessageType::trigger);
        triggered_ = true;

        frame = read_frame(asynchronous_);
        CHECK(frame.type == MessageType::async_lock);
        CHECK(frame.control == 1);
        CHECK(frame.payload.empty());
        write_frame(asynchronous_, MessageType::async_lock_response, 1, 0);

        frame = read_frame(asynchronous_);
        CHECK(frame.type == MessageType::async_lock);
        CHECK(frame.control == 0);
        write_frame(asynchronous_, MessageType::async_lock_response, 1, 0);

        frame = read_frame(asynchronous_);
        CHECK(frame.type == MessageType::async_lock);
        CHECK(frame.control == 1);
        CHECK(!frame.payload.empty());
        write_frame(asynchronous_, MessageType::async_lock_response, 1, 0);

        frame = read_frame(asynchronous_);
        CHECK(frame.type == MessageType::async_lock);
        CHECK(frame.control == 0);
        write_frame(asynchronous_, MessageType::async_lock_response, 2, 0);

        frame = read_frame(asynchronous_);
        CHECK(frame.type == MessageType::async_device_clear);
        write_frame(asynchronous_, MessageType::async_device_clear_acknowledge, 0,
                    0);
        frame = read_frame(synchronous_);
        CHECK(frame.type == MessageType::device_clear_complete);
        write_frame(synchronous_, MessageType::device_clear_acknowledge, 0, 0);

        frame = read_frame(synchronous_);
        CHECK(frame.type == MessageType::data_end);
        CHECK(std::string(frame.payload.begin(), frame.payload.end()) == "HOLD\n");
        const auto blocked_message_id = frame.parameter;
        frame = read_frame(asynchronous_);
        CHECK(frame.type == MessageType::async_device_clear);
        write_frame(asynchronous_, MessageType::async_device_clear_acknowledge, 0,
                    0);
        write_frame(synchronous_, MessageType::interrupted, 0,
                    blocked_message_id);
        frame = read_frame(synchronous_);
        CHECK(frame.type == MessageType::device_clear_complete);
        write_frame(synchronous_, MessageType::device_clear_acknowledge, 0, 0);

        frame = read_frame(synchronous_);
        CHECK(frame.type == MessageType::data_end);
        CHECK(std::string(frame.payload.begin(), frame.payload.end()) == "AGAIN\n");
        write_frame(synchronous_, MessageType::data_end, 0, frame.parameter,
                    "AGAIN-OK\n");

        frame = read_frame(synchronous_);
        CHECK(frame.type == MessageType::data_end);
        CHECK(std::string(frame.payload.begin(), frame.payload.end()) ==
              "TIMEOUT\n");
        const auto timeout_message_id = frame.parameter;
        {
            std::lock_guard lock(timeout_mutex_);
            timeout_request_received_ = true;
        }
        timeout_condition_.notify_one();
        frame = read_frame(asynchronous_);
        CHECK(frame.type == MessageType::async_device_clear);
        write_frame(asynchronous_, MessageType::async_device_clear_acknowledge, 0,
                    0);
        write_frame(synchronous_, MessageType::interrupted, 0,
                    timeout_message_id);
        frame = read_frame(synchronous_);
        CHECK(frame.type == MessageType::device_clear_complete);
        write_frame(synchronous_, MessageType::device_clear_acknowledge, 0, 0);

        frame = read_frame(synchronous_);
        CHECK(frame.type == MessageType::data_end);
        CHECK(std::string(frame.payload.begin(), frame.payload.end()) == "AFTER\n");
        write_frame(synchronous_, MessageType::data_end, 0, frame.parameter,
                    "AFTER-OK\n");
    }

    asio::io_context context_;
    asio::ip::tcp::acceptor acceptor_;
    asio::ip::tcp::socket synchronous_{context_};
    asio::ip::tcp::socket asynchronous_{context_};
    std::thread worker_;
    bool triggered_{false};
    std::mutex timeout_mutex_;
    std::condition_variable timeout_condition_;
    bool timeout_request_received_{false};
};

class UnsupportedModeServer final {
public:
    UnsupportedModeServer()
        : acceptor_(context_, asio::ip::tcp::endpoint(
                                  asio::ip::address_v4::loopback(), 0)) {
        worker_ = std::thread([this] {
            asio::ip::tcp::socket socket(context_);
            asio::error_code error;
            acceptor_.accept(socket, error);
            CHECK(!error);
            const auto frame = read_frame(socket);
            CHECK(frame.type == MessageType::initialize);
            write_frame(socket, MessageType::initialize_response, 1,
                        (UINT32_C(0x0100) << 16u) | 7u);
        });
    }

    ~UnsupportedModeServer() {
        asio::error_code ignored;
        acceptor_.close(ignored);
        if (worker_.joinable()) {
            worker_.join();
        }
    }

    std::uint16_t port() const { return acceptor_.local_endpoint().port(); }

private:
    asio::io_context context_;
    asio::ip::tcp::acceptor acceptor_;
    std::thread worker_;
};

class FatalClearServer final {
public:
    FatalClearServer()
        : acceptor_(context_, asio::ip::tcp::endpoint(
                                  asio::ip::address_v4::loopback(), 0)) {
        worker_ = std::thread([this] { run(); });
    }

    ~FatalClearServer() {
        asio::error_code ignored;
        acceptor_.close(ignored);
        synchronous_.close(ignored);
        asynchronous_.close(ignored);
        if (worker_.joinable()) {
            worker_.join();
        }
    }

    std::uint16_t port() const { return acceptor_.local_endpoint().port(); }

private:
    void run() {
        asio::error_code error;
        acceptor_.accept(synchronous_, error);
        CHECK(!error);
        auto frame = read_frame(synchronous_);
        CHECK(frame.type == MessageType::initialize);
        write_frame(synchronous_, MessageType::initialize_response, 0,
                    (UINT32_C(0x0100) << 16u) | UINT32_C(0x2345));

        acceptor_.accept(asynchronous_, error);
        CHECK(!error);
        frame = read_frame(asynchronous_);
        CHECK(frame.type == MessageType::async_initialize);
        write_frame(asynchronous_, MessageType::async_initialize_response, 0, 0);
        frame = read_frame(asynchronous_);
        CHECK(frame.type == MessageType::async_maximum_message_size);
        const std::string maximum("\0\0\0\0\0\0\x10\0", 8);
        write_frame(asynchronous_,
                    MessageType::async_maximum_message_size_response, 0, 0,
                    maximum);

        frame = read_frame(synchronous_);
        CHECK(frame.type == MessageType::data_end);
        CHECK(std::string(frame.payload.begin(), frame.payload.end()) ==
              "FATAL\n");
        frame = read_frame(asynchronous_);
        CHECK(frame.type == MessageType::async_device_clear);
        write_frame(asynchronous_, MessageType::fatal_error, 0, 0);
    }

    asio::io_context context_;
    asio::ip::tcp::acceptor acceptor_;
    asio::ip::tcp::socket synchronous_{context_};
    asio::ip::tcp::socket asynchronous_{context_};
    std::thread worker_;
};

ViStatus write_text(ViSession session, const std::string& text) {
    ViUInt32 count = 0;
    const auto status = viWrite(session, reinterpret_cast<ViConstBuf>(text.data()),
                                static_cast<ViUInt32>(text.size()), &count);
    CHECK(status < VI_SUCCESS || count == text.size());
    return status;
}

}  // namespace

int main() {
    HiSlipSimulator simulator;
    ViSession rm = VI_NULL;
    CHECK(viOpenDefaultRM(&rm) == VI_SUCCESS);
    CHECK(wrvisaSetTcpipServicePort(rm, "127.0.0.1",
                                    WRVISA_TCPIP_PROTOCOL_HISLIP,
                                    simulator.port()) == VI_SUCCESS);
    ViSession session = VI_NULL;
    CHECK(viOpen(rm, "TCPIP0::127.0.0.1::hislip0::INSTR", VI_NO_LOCK, 1000,
                 &session) == VI_SUCCESS);
    CHECK(session != VI_NULL);

    std::array<ViChar, VI_FIND_BUFLEN> address{};
    ViUInt16 port = 0;
    CHECK(viGetAttribute(session, VI_ATTR_TCPIP_ADDR, address.data()) == VI_SUCCESS);
    CHECK(std::string(address.data()) == "127.0.0.1");
    CHECK(viGetAttribute(session, VI_ATTR_TCPIP_PORT, &port) == VI_SUCCESS);
    CHECK(port == simulator.port());
    CHECK(viSetAttribute(session, VI_ATTR_TMO_VALUE, 1000) == VI_SUCCESS);
    CHECK(viSetAttribute(session, VI_ATTR_TERMCHAR, '\n') == VI_SUCCESS);
    CHECK(viSetAttribute(session, VI_ATTR_TERMCHAR_EN, VI_TRUE) == VI_SUCCESS);

    CHECK(write_text(session, "*IDN?\n") == VI_SUCCESS);
    std::array<ViByte, 64> buffer{};
    ViUInt32 count = 0;
    CHECK(viRead(session, buffer.data(), buffer.size(), &count) ==
          VI_SUCCESS_TERM_CHAR);
    CHECK(std::string(reinterpret_cast<char*>(buffer.data()), count) ==
          "HISLIP-OK\n");

    CHECK(write_text(session, "SPLIT\n") == VI_SUCCESS);
    CHECK(viRead(session, buffer.data(), 2, &count) == VI_SUCCESS_MAX_CNT);
    CHECK(std::string(reinterpret_cast<char*>(buffer.data()), count) == "AB");
    CHECK(viRead(session, buffer.data(), buffer.size(), &count) ==
          VI_SUCCESS_TERM_CHAR);
    CHECK(std::string(reinterpret_cast<char*>(buffer.data()), count) == "CD\n");

    ViUInt16 status_byte = 0;
    CHECK(viReadSTB(session, &status_byte) == VI_SUCCESS);
    CHECK(status_byte == 0x42);
    CHECK(viAssertTrigger(session, VI_TRIG_PROT_DEFAULT) == VI_SUCCESS);

    CHECK(viLock(session, VI_EXCLUSIVE_LOCK, 1000, nullptr, nullptr) == VI_SUCCESS);
    CHECK(viUnlock(session) == VI_SUCCESS);
    std::array<ViChar, VI_FIND_BUFLEN> access_key{};
    CHECK(viLock(session, VI_SHARED_LOCK, 1000, nullptr, access_key.data()) ==
          VI_SUCCESS);
    CHECK(access_key[0] != '\0');
    CHECK(viUnlock(session) == VI_SUCCESS);
    CHECK(viClear(session) == VI_SUCCESS);

    CHECK(write_text(session, "HOLD\n") == VI_SUCCESS);
    CHECK(viSetAttribute(session, VI_ATTR_TMO_VALUE, 5000) == VI_SUCCESS);
    ViUInt32 blocked_count = 99;
    auto blocked = std::async(std::launch::async, [&] {
        return viRead(session, buffer.data(), buffer.size(), &blocked_count);
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    CHECK(viTerminate(session, VI_NULL, VI_NULL) == VI_SUCCESS);
    const auto blocked_status = blocked.get();
    if (blocked_status != VI_ERROR_ABORT) {
        std::cerr << "blocked HiSLIP read status=" << blocked_status << '\n';
    }
    CHECK(blocked_status == VI_ERROR_ABORT);
    CHECK(blocked_count == 0);
    CHECK(viSetAttribute(session, VI_ATTR_TMO_VALUE, 1000) == VI_SUCCESS);
    CHECK(write_text(session, "AGAIN\n") == VI_SUCCESS);
    CHECK(viRead(session, buffer.data(), buffer.size(), &count) ==
          VI_SUCCESS_TERM_CHAR);
    CHECK(std::string(reinterpret_cast<char*>(buffer.data()), count) ==
          "AGAIN-OK\n");
    CHECK(write_text(session, "TIMEOUT\n") == VI_SUCCESS);
    CHECK(simulator.wait_for_timeout_request());
    CHECK(viSetAttribute(session, VI_ATTR_TMO_VALUE, 30) == VI_SUCCESS);
    count = 99;
    const auto timeout_status =
        viRead(session, buffer.data(), buffer.size(), &count);
    if (timeout_status != VI_ERROR_TMO) {
        std::cerr << "HiSLIP timeout read status=" << timeout_status << '\n';
    }
    CHECK(timeout_status == VI_ERROR_TMO);
    CHECK(count == 0);
    CHECK(viSetAttribute(session, VI_ATTR_TMO_VALUE, 1000) == VI_SUCCESS);
    CHECK(write_text(session, "AFTER\n") == VI_SUCCESS);
    CHECK(viRead(session, buffer.data(), buffer.size(), &count) ==
          VI_SUCCESS_TERM_CHAR);
    CHECK(std::string(reinterpret_cast<char*>(buffer.data()), count) ==
          "AFTER-OK\n");
    CHECK(viFlush(session, 0) == VI_ERROR_INV_MASK);
    CHECK(simulator.triggered());
    CHECK(viClose(session) == VI_SUCCESS);
    CHECK(viClose(rm) == VI_SUCCESS);

    UnsupportedModeServer unsupported;
    CHECK(viOpenDefaultRM(&rm) == VI_SUCCESS);
    CHECK(wrvisaSetTcpipServicePort(rm, "127.0.0.1",
                                    WRVISA_TCPIP_PROTOCOL_HISLIP,
                                    unsupported.port()) == VI_SUCCESS);
    session = 123;
    CHECK(viOpen(rm, "TCPIP0::127.0.0.1::hislip0::INSTR", VI_NO_LOCK, 1000,
                 &session) == VI_ERROR_NSUP_OPER);
    CHECK(session == VI_NULL);
    CHECK(viClose(rm) == VI_SUCCESS);

    FatalClearServer fatal_clear;
    CHECK(viOpenDefaultRM(&rm) == VI_SUCCESS);
    CHECK(wrvisaSetTcpipServicePort(rm, "127.0.0.1",
                                    WRVISA_TCPIP_PROTOCOL_HISLIP,
                                    fatal_clear.port()) == VI_SUCCESS);
    CHECK(viOpen(rm, "TCPIP0::127.0.0.1::hislip0::INSTR", VI_NO_LOCK, 1000,
                 &session) == VI_SUCCESS);
    CHECK(write_text(session, "FATAL\n") == VI_SUCCESS);
    CHECK(viSetAttribute(session, VI_ATTR_TMO_VALUE, 5000) == VI_SUCCESS);
    count = 99;
    auto fatal_read = std::async(std::launch::async, [&] {
        return viRead(session, buffer.data(), buffer.size(), &count);
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    CHECK(viTerminate(session, VI_NULL, VI_NULL) == VI_SUCCESS);
    CHECK(fatal_read.wait_for(std::chrono::seconds(2)) ==
          std::future_status::ready);
    CHECK(fatal_read.get() == VI_ERROR_ABORT);
    CHECK(count == 0);
    CHECK(viClose(session) == VI_SUCCESS);
    CHECK(viClose(rm) == VI_SUCCESS);
    return 0;
}
