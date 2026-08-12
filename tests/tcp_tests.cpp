#include <array>
#include <chrono>
#include <condition_variable>
#include <future>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <asio.hpp>

#include "core/handle_table.h"
#include "core/objects.h"
#include "test_support.h"
#include "visa.h"

namespace {

class TcpTestServer {
public:
    TcpTestServer()
        : acceptor_(context_) {
        asio::error_code error;
        acceptor_.open(asio::ip::tcp::v4(), error);
        CHECK(!error);
        acceptor_.bind(
            asio::ip::tcp::endpoint(asio::ip::address_v4::loopback(), 0), error);
        CHECK(!error);
        acceptor_.listen(asio::socket_base::max_listen_connections, error);
        CHECK(!error);
        worker_ = std::thread([this] { run(); });
    }

    ~TcpTestServer() {
        asio::error_code ignored;
        acceptor_.close(ignored);
        if (worker_.joinable()) {
            worker_.join();
        }
    }

    unsigned short port() const { return acceptor_.local_endpoint().port(); }

private:
    void run() {
        asio::ip::tcp::socket client(context_);
        asio::error_code error;
        acceptor_.accept(client, error);
        std::string incoming;
        while (!error) {
            asio::read_until(client, asio::dynamic_buffer(incoming), '\n', error);
            if (error) {
                break;
            }
            const auto newline = incoming.find('\n');
            const auto command = incoming.substr(0, newline + 1u);
            incoming.erase(0, newline + 1u);
            if (command == "FIRST\n") {
                asio::write(client, asio::buffer("TCP-OK\nEXTRA", 12), error);
            } else if (command == "PARTIAL\n") {
                asio::write(client, asio::buffer("AB", 2), error);
            } else if (command == "AGAIN\n") {
                asio::write(client, asio::buffer("AGAIN-OK\n", 9), error);
            } else if (command != "HOLD\n") {
                asio::write(client, asio::buffer(command), error);
            }
        }
    }

    asio::io_context context_;
    asio::ip::tcp::acceptor acceptor_;
    std::thread worker_;
};

class TcpNoReadServer {
public:
    TcpNoReadServer()
        : acceptor_(context_), client_(context_) {
        asio::error_code error;
        acceptor_.open(asio::ip::tcp::v4(), error);
        CHECK(!error);
        acceptor_.bind(
            asio::ip::tcp::endpoint(asio::ip::address_v4::loopback(), 0), error);
        CHECK(!error);
        acceptor_.listen(asio::socket_base::max_listen_connections, error);
        CHECK(!error);
        worker_ = std::thread([this] { run(); });
    }

    ~TcpNoReadServer() {
        {
            std::lock_guard lock(mutex_);
            stop_ = true;
        }
        condition_.notify_all();
        asio::error_code ignored;
        client_.close(ignored);
        acceptor_.close(ignored);
        if (worker_.joinable()) {
            worker_.join();
        }
    }

    unsigned short port() const { return acceptor_.local_endpoint().port(); }

private:
    void run() {
        asio::error_code error;
        acceptor_.accept(client_, error);
        if (!error) {
            asio::write(client_, asio::buffer("READY\n", 6), error);
        }
        std::unique_lock lock(mutex_);
        condition_.wait(lock, [this] { return stop_; });
    }

    asio::io_context context_;
    asio::ip::tcp::acceptor acceptor_;
    asio::ip::tcp::socket client_;
    std::mutex mutex_;
    std::condition_variable condition_;
    bool stop_{false};
    std::thread worker_;
};

class TcpCloseAfterWriteServer {
public:
    TcpCloseAfterWriteServer()
        : acceptor_(context_) {
        asio::error_code error;
        acceptor_.open(asio::ip::tcp::v4(), error);
        CHECK(!error);
        acceptor_.bind(
            asio::ip::tcp::endpoint(asio::ip::address_v4::loopback(), 0), error);
        CHECK(!error);
        acceptor_.listen(asio::socket_base::max_listen_connections, error);
        CHECK(!error);
        worker_ = std::thread([this] { run(); });
    }

    ~TcpCloseAfterWriteServer() {
        asio::error_code ignored;
        acceptor_.close(ignored);
        if (worker_.joinable()) {
            worker_.join();
        }
    }

    unsigned short port() const { return acceptor_.local_endpoint().port(); }

private:
    void run() {
        asio::ip::tcp::socket client(context_);
        asio::error_code error;
        acceptor_.accept(client, error);
        if (!error) {
            asio::write(client, asio::buffer("END\nTAIL", 8), error);
        }
        client.shutdown(asio::ip::tcp::socket::shutdown_both, error);
        client.close(error);
    }

    asio::io_context context_;
    asio::ip::tcp::acceptor acceptor_;
    std::thread worker_;
};

std::shared_ptr<wrvisa::SessionObject> session_object(ViSession session) {
    auto object = wrvisa::get_handle<wrvisa::SessionObject>(
        session, wrvisa::ObjectType::session);
    CHECK(object != nullptr);
    return object;
}

void wait_until_active(const std::shared_ptr<wrvisa::SessionObject>& session,
                       std::size_t expected = 1) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    while (session->active_operation_count() < expected &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::yield();
    }
    CHECK(session->active_operation_count() >= expected);
}

ViStatus write_text(ViSession session, const std::string& text) {
    ViUInt32 count = 0;
    const auto status = viWrite(session,
                                reinterpret_cast<ViConstBuf>(text.data()),
                                static_cast<ViUInt32>(text.size()), &count);
    CHECK(status < VI_SUCCESS || count == text.size());
    return status;
}

}  // namespace

int main() {
    using namespace std::chrono_literals;

    TcpTestServer server;
    ViSession rm = VI_NULL;
    CHECK(viOpenDefaultRM(&rm) == VI_SUCCESS);
    asio::io_context refused_context;
    asio::ip::tcp::acceptor refused_acceptor(
        refused_context,
        asio::ip::tcp::endpoint(asio::ip::address_v4::loopback(), 0));
    const auto refused_port = refused_acceptor.local_endpoint().port();
    refused_acceptor.close();
    const auto refused_resource =
        "TCPIP0::127.0.0.1::" + std::to_string(refused_port) + "::SOCKET";
    // On Linux, connecting to a closed loopback port is refused instantly; on
    // Windows the select-reactor notices the refusal only after ~2s (two SYN
    // retransmits, observed 2044-2049ms on Windows 11), so a 1000ms open
    // deadline fires first and returns VI_ERROR_TMO instead. Give Windows a
    // longer deadline so the refusal mapping itself is what gets asserted.
#ifdef _WIN32
    constexpr ViUInt32 refused_timeout = 5000;
#else
    constexpr ViUInt32 refused_timeout = 1000;
#endif
    ViSession refused_session = 123;
    CHECK(viOpen(rm, refused_resource.c_str(), VI_NO_LOCK, refused_timeout,
                 &refused_session) == VI_ERROR_RSRC_NFOUND);
    CHECK(refused_session == VI_NULL);

    const auto resource = "TCPIP0::127.0.0.1::" + std::to_string(server.port()) +
                          "::SOCKET";
    ViSession session = VI_NULL;
    CHECK(viOpen(rm, resource.c_str(), VI_NO_LOCK, 1000, &session) == VI_SUCCESS);
    CHECK(session != VI_NULL);

    std::array<ViChar, VI_FIND_BUFLEN> hostname{};
    std::array<ViChar, VI_FIND_BUFLEN> address{};
    ViUInt16 port = 0;
    CHECK(viGetAttribute(session, VI_ATTR_TCPIP_HOSTNAME, hostname.data()) == VI_SUCCESS);
    CHECK(std::string(hostname.data()) == "127.0.0.1");
    CHECK(viGetAttribute(session, VI_ATTR_TCPIP_ADDR, address.data()) == VI_SUCCESS);
    CHECK(std::string(address.data()) == "127.0.0.1");
    CHECK(viGetAttribute(session, VI_ATTR_TCPIP_PORT, &port) == VI_SUCCESS);
    CHECK(port == server.port());
    CHECK(viSetAttribute(session, VI_ATTR_TCPIP_NODELAY, VI_TRUE) == VI_SUCCESS);
    ViBoolean enabled = VI_FALSE;
    CHECK(viGetAttribute(session, VI_ATTR_TCPIP_NODELAY, &enabled) == VI_SUCCESS);
    CHECK(enabled == VI_TRUE);

    CHECK(viSetAttribute(session, VI_ATTR_TERMCHAR, '\n') == VI_SUCCESS);
    CHECK(viSetAttribute(session, VI_ATTR_TERMCHAR_EN, VI_TRUE) == VI_SUCCESS);
    CHECK(write_text(session, "FIRST\n") == VI_SUCCESS);

    std::array<ViByte, 64> buffer{};
    ViUInt32 count = 0;
    CHECK(viRead(session, buffer.data(), static_cast<ViUInt32>(buffer.size()), &count) ==
          VI_SUCCESS_TERM_CHAR);
    CHECK(std::string(reinterpret_cast<char*>(buffer.data()), count) == "TCP-OK\n");
    CHECK(viRead(session, buffer.data(), 5, &count) == VI_SUCCESS_MAX_CNT);
    CHECK(std::string(reinterpret_cast<char*>(buffer.data()), count) == "EXTRA");

    CHECK(viSetAttribute(session, VI_ATTR_TMO_VALUE, 30) == VI_SUCCESS);
    CHECK(write_text(session, "PARTIAL\n") == VI_SUCCESS);
    count = 99;
    CHECK(viRead(session, buffer.data(), 4, &count) == VI_ERROR_TMO);
    CHECK(count == 0);
    CHECK(viRead(session, buffer.data(), 2, &count) == VI_SUCCESS_MAX_CNT);
    CHECK(std::string(reinterpret_cast<char*>(buffer.data()), count) == "AB");

    CHECK(viSetAttribute(session, VI_ATTR_TMO_VALUE, 5000) == VI_SUCCESS);
    CHECK(write_text(session, "HOLD\n") == VI_SUCCESS);
    auto object = session_object(session);
    ViUInt32 queue_head_count = 73;
    auto queue_head = std::async(std::launch::async, [&] {
        return viRead(session, buffer.data(), 4, &queue_head_count);
    });
    wait_until_active(object);
    CHECK(viSetAttribute(session, VI_ATTR_TMO_VALUE, 30) == VI_SUCCESS);
    ViUInt32 queued_timeout_count = 74;
    std::array<ViByte, 4> queued_timeout_buffer{};
    auto queued_timeout = std::async(std::launch::async, [&] {
        return viRead(session, queued_timeout_buffer.data(), 4,
                      &queued_timeout_count);
    });
    wait_until_active(object, 2);
    CHECK(queued_timeout.wait_for(1s) == std::future_status::ready);
    CHECK(queued_timeout.get() == VI_ERROR_TMO);
    CHECK(queued_timeout_count == 0);
    CHECK(queue_head.wait_for(100ms) == std::future_status::timeout);
    CHECK(viTerminate(session, VI_NULL, VI_NULL) == VI_SUCCESS);
    CHECK(queue_head.wait_for(1s) == std::future_status::ready);
    CHECK(queue_head.get() == VI_ERROR_ABORT);
    CHECK(queue_head_count == 0);

    CHECK(viSetAttribute(session, VI_ATTR_TMO_VALUE, 5000) == VI_SUCCESS);
    CHECK(write_text(session, "HOLD\n") == VI_SUCCESS);
    std::array<ViByte, 4> second_buffer{};
    ViUInt32 first_count = 77;
    ViUInt32 second_count = 88;
    auto first_blocked = std::async(std::launch::async, [&] {
        return viRead(session, buffer.data(), 4, &first_count);
    });
    auto second_blocked = std::async(std::launch::async, [&] {
        return viRead(session, second_buffer.data(), 4, &second_count);
    });
    wait_until_active(object, 2);
    CHECK(viTerminate(session, VI_NULL, VI_NULL) == VI_SUCCESS);
    CHECK(first_blocked.wait_for(1s) == std::future_status::ready);
    CHECK(second_blocked.wait_for(1s) == std::future_status::ready);
    CHECK(first_blocked.get() == VI_ERROR_ABORT);
    CHECK(second_blocked.get() == VI_ERROR_ABORT);
    CHECK(first_count == 0);
    CHECK(second_count == 0);

    CHECK(write_text(session, "AGAIN\n") == VI_SUCCESS);
    CHECK(viRead(session, buffer.data(), static_cast<ViUInt32>(buffer.size()), &count) ==
          VI_SUCCESS_TERM_CHAR);
    CHECK(std::string(reinterpret_cast<char*>(buffer.data()), count) == "AGAIN-OK\n");

    ViUInt16 stb = 0;
    CHECK(viReadSTB(session, &stb) == VI_ERROR_NSUP_OPER);
    CHECK(viAssertTrigger(session, VI_TRIG_PROT_DEFAULT) == VI_ERROR_NSUP_OPER);
    CHECK(viClear(session) == VI_ERROR_NSUP_OPER);

    CHECK(write_text(session, "HOLD\n") == VI_SUCCESS);
    count = 66;
    auto close_blocked = std::async(std::launch::async, [&] {
        return viRead(session, buffer.data(), 4, &count);
    });
    wait_until_active(object);
    CHECK(viClose(session) == VI_SUCCESS);
    CHECK(close_blocked.wait_for(1s) == std::future_status::ready);
    CHECK(close_blocked.get() == VI_ERROR_ABORT);
    CHECK(count == 0);

    TcpNoReadServer no_read_server;
    const auto no_read_resource =
        "TCPIP0::127.0.0.1::" + std::to_string(no_read_server.port()) + "::SOCKET";
    ViSession write_session = VI_NULL;
    CHECK(viOpen(rm, no_read_resource.c_str(), VI_NO_LOCK, 1000, &write_session) ==
          VI_SUCCESS);
    CHECK(viSetAttribute(write_session, VI_ATTR_TERMCHAR, '\n') == VI_SUCCESS);
    CHECK(viSetAttribute(write_session, VI_ATTR_TERMCHAR_EN, VI_TRUE) == VI_SUCCESS);
    CHECK(viRead(write_session, buffer.data(), static_cast<ViUInt32>(buffer.size()),
                 &count) == VI_SUCCESS_TERM_CHAR);
    CHECK(std::string(reinterpret_cast<char*>(buffer.data()), count) == "READY\n");
    CHECK(viSetAttribute(write_session, VI_ATTR_TMO_VALUE, 5000) == VI_SUCCESS);
    auto write_object = session_object(write_session);
    std::vector<ViByte> large_write(16u * 1024u * 1024u, 0x5A);
    ViUInt32 large_count = 91;
    auto blocked_write = std::async(std::launch::async, [&] {
        return viWrite(write_session, large_write.data(),
                       static_cast<ViUInt32>(large_write.size()), &large_count);
    });
    wait_until_active(write_object);
    CHECK(blocked_write.wait_for(100ms) == std::future_status::timeout);
    CHECK(viSetAttribute(write_session, VI_ATTR_TMO_VALUE, 30) == VI_SUCCESS);
    const std::array<ViByte, 1> queued_byte{0x33};
    ViUInt32 queued_write_count = 92;
    auto queued_write = std::async(std::launch::async, [&] {
        return viWrite(write_session, queued_byte.data(), 1, &queued_write_count);
    });
    wait_until_active(write_object, 2);
    CHECK(queued_write.wait_for(1s) == std::future_status::ready);
    CHECK(queued_write.get() == VI_ERROR_TMO);
    CHECK(queued_write_count == 0);
    CHECK(viTerminate(write_session, VI_NULL, VI_NULL) == VI_SUCCESS);
    CHECK(blocked_write.wait_for(1s) == std::future_status::ready);
    CHECK(blocked_write.get() == VI_ERROR_ABORT);
    CHECK(large_count == 0);
    CHECK(viClose(write_session) == VI_SUCCESS);

    TcpCloseAfterWriteServer close_after_write_server;
    const auto close_after_write_resource =
        "TCPIP0::127.0.0.1::" + std::to_string(close_after_write_server.port()) +
        "::SOCKET";
    ViSession close_after_write_session = VI_NULL;
    CHECK(viOpen(rm, close_after_write_resource.c_str(), VI_NO_LOCK, 1000,
                 &close_after_write_session) == VI_SUCCESS);
    CHECK(viSetAttribute(close_after_write_session, VI_ATTR_TERMCHAR, '\n') ==
          VI_SUCCESS);
    CHECK(viSetAttribute(close_after_write_session, VI_ATTR_TERMCHAR_EN, VI_TRUE) ==
          VI_SUCCESS);
    CHECK(viRead(close_after_write_session, buffer.data(),
                 static_cast<ViUInt32>(buffer.size()), &count) ==
          VI_SUCCESS_TERM_CHAR);
    CHECK(std::string(reinterpret_cast<char*>(buffer.data()), count) == "END\n");
    CHECK(viRead(close_after_write_session, buffer.data(), 4, &count) ==
          VI_SUCCESS_MAX_CNT);
    CHECK(std::string(reinterpret_cast<char*>(buffer.data()), count) == "TAIL");
    CHECK(viRead(close_after_write_session, buffer.data(), 1, &count) ==
          VI_ERROR_CONN_LOST);
    CHECK(count == 0);
    CHECK(viClose(close_after_write_session) == VI_SUCCESS);
    CHECK(viClose(rm) == VI_SUCCESS);
    return 0;
}
