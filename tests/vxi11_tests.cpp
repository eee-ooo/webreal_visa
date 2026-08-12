#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <condition_variable>
#include <future>
#include <mutex>
#include <span>
#include <string>
#include <thread>
#include <vector>

#include <asio.hpp>

#include "backends/vxi11/vxi11_protocol.h"
#include "test_support.h"
#include "visa.h"
#include "webreal_visa_ext.h"

namespace {

struct RpcCall {
    std::uint32_t xid{0};
    std::uint32_t program{0};
    std::uint32_t version{0};
    std::uint32_t procedure{0};
    std::vector<std::uint8_t> arguments;
};

std::uint32_t read_u32(const std::uint8_t* input) {
    return (static_cast<std::uint32_t>(input[0]) << 24u) |
           (static_cast<std::uint32_t>(input[1]) << 16u) |
           (static_cast<std::uint32_t>(input[2]) << 8u) | input[3];
}

std::vector<std::uint8_t> read_record(asio::ip::tcp::socket& socket) {
    std::vector<std::uint8_t> message;
    bool last = false;
    asio::error_code error;
    while (!last) {
        std::array<std::uint8_t, 4> marker{};
        asio::read(socket, asio::buffer(marker), error);
        CHECK(!error);
        const auto value = read_u32(marker.data());
        last = (value & UINT32_C(0x80000000)) != 0;
        const auto size = static_cast<std::size_t>(value & UINT32_C(0x7FFFFFFF));
        CHECK(size <= 16u * 1024u * 1024u);
        const auto offset = message.size();
        message.resize(offset + size);
        if (size != 0) {
            asio::read(socket, asio::buffer(message.data() + offset, size), error);
            CHECK(!error);
        }
    }
    return message;
}

RpcCall read_call(asio::ip::tcp::socket& socket) {
    const auto bytes = read_record(socket);
    wrvisa::vxi11::XdrReader reader(bytes);
    RpcCall call;
    std::uint32_t message_type = 0;
    std::uint32_t rpc_version = 0;
    std::uint32_t flavor = 0;
    std::uint32_t length = 0;
    CHECK(reader.u32(call.xid));
    CHECK(reader.u32(message_type));
    CHECK(message_type == 0);
    CHECK(reader.u32(rpc_version));
    CHECK(rpc_version == 2);
    CHECK(reader.u32(call.program));
    CHECK(reader.u32(call.version));
    CHECK(reader.u32(call.procedure));
    CHECK(reader.u32(flavor));
    CHECK(reader.u32(length));
    CHECK(flavor == 0 && length == 0);
    CHECK(reader.u32(flavor));
    CHECK(reader.u32(length));
    CHECK(flavor == 0 && length == 0);
    const auto arguments = reader.remaining();
    call.arguments.assign(arguments.begin(), arguments.end());
    return call;
}

void write_reply(asio::ip::tcp::socket& socket, const RpcCall& call,
                 const wrvisa::vxi11::XdrWriter& result) {
    wrvisa::vxi11::XdrWriter reply;
    reply.u32(call.xid);
    reply.u32(1);  // REPLY
    reply.u32(0);  // accepted
    reply.u32(0);  // AUTH_NONE verifier
    reply.u32(0);
    reply.u32(0);  // SUCCESS
    auto body = reply.take();
    body.insert(body.end(), result.bytes().begin(), result.bytes().end());
    const auto marker = UINT32_C(0x80000000) |
                        static_cast<std::uint32_t>(body.size());
    const std::array<std::uint8_t, 4> header{{
        static_cast<std::uint8_t>(marker >> 24u),
        static_cast<std::uint8_t>(marker >> 16u),
        static_cast<std::uint8_t>(marker >> 8u),
        static_cast<std::uint8_t>(marker),
    }};
    std::vector<asio::const_buffer> buffers{asio::buffer(header), asio::buffer(body)};
    asio::error_code error;
    asio::write(socket, buffers, error);
    CHECK(!error);
}

class Vxi11Simulator final {
public:
    Vxi11Simulator()
        : portmapper_acceptor_(context_, loopback_endpoint()),
          core_acceptor_(context_, loopback_endpoint()),
          abort_acceptor_(context_, loopback_endpoint()) {
        portmapper_worker_ = std::thread([this] { run_portmapper(); });
        core_worker_ = std::thread([this] { run_core(); });
        abort_worker_ = std::thread([this] { run_abort(); });
    }

    ~Vxi11Simulator() {
        asio::error_code ignored;
        portmapper_acceptor_.close(ignored);
        core_acceptor_.close(ignored);
        abort_acceptor_.close(ignored);
        portmapper_socket_.close(ignored);
        core_socket_.close(ignored);
        abort_socket_.close(ignored);
        if (portmapper_worker_.joinable()) {
            portmapper_worker_.join();
        }
        if (core_worker_.joinable()) {
            core_worker_.join();
        }
        if (abort_worker_.joinable()) {
            abort_worker_.join();
        }
    }

    std::uint16_t portmapper_port() const {
        return portmapper_acceptor_.local_endpoint().port();
    }
    std::uint16_t core_port() const {
        return core_acceptor_.local_endpoint().port();
    }
    bool triggered() const noexcept { return triggered_; }

private:
    static asio::ip::tcp::endpoint loopback_endpoint() {
        return {asio::ip::address_v4::loopback(), 0};
    }

    void run_portmapper() {
        asio::error_code error;
        portmapper_acceptor_.accept(portmapper_socket_, error);
        CHECK(!error);
        const auto call = read_call(portmapper_socket_);
        CHECK(call.program == wrvisa::vxi11::kPortmapperProgram);
        CHECK(call.version == wrvisa::vxi11::kPortmapperVersion);
        CHECK(call.procedure == wrvisa::vxi11::kPortmapperGetPort);
        wrvisa::vxi11::XdrReader arguments(call.arguments);
        std::uint32_t program = 0;
        std::uint32_t version = 0;
        std::uint32_t protocol = 0;
        std::uint32_t port = 0;
        CHECK(arguments.u32(program));
        CHECK(arguments.u32(version));
        CHECK(arguments.u32(protocol));
        CHECK(arguments.u32(port));
        CHECK(program == wrvisa::vxi11::kDeviceCoreProgram);
        CHECK(version == 1 && protocol == 6 && port == 0);
        wrvisa::vxi11::XdrWriter result;
        result.u32(core_port());
        write_reply(portmapper_socket_, call, result);
    }

    void run_abort() {
        asio::error_code error;
        abort_acceptor_.accept(abort_socket_, error);
        CHECK(!error);
        for (;;) {
            std::array<std::uint8_t, 1> probe{};
            abort_socket_.receive(asio::buffer(probe),
                                  asio::socket_base::message_peek, error);
            if (error) {
                return;
            }
            const auto call = read_call(abort_socket_);
            CHECK(call.program == wrvisa::vxi11::kDeviceAsyncProgram);
            CHECK(call.procedure == wrvisa::vxi11::kDeviceAbort);
            {
                std::lock_guard lock(abort_mutex_);
                aborted_ = true;
            }
            abort_condition_.notify_all();
            wrvisa::vxi11::XdrWriter result;
            result.u32(0);
            write_reply(abort_socket_, call, result);
        }
    }

    void run_core() {
        asio::error_code error;
        core_acceptor_.accept(core_socket_, error);
        CHECK(!error);
        for (;;) {
            const auto call = read_call(core_socket_);
            CHECK(call.program == wrvisa::vxi11::kDeviceCoreProgram);
            wrvisa::vxi11::XdrReader arguments(call.arguments);
            wrvisa::vxi11::XdrWriter result;
            if (call.procedure == wrvisa::vxi11::kCreateLink) {
                std::uint32_t client = 0;
                std::uint32_t lock_device = 0;
                std::uint32_t timeout = 0;
                std::vector<std::uint8_t> device;
                CHECK(arguments.u32(client));
                CHECK(arguments.u32(lock_device));
                CHECK(arguments.u32(timeout));
                CHECK(arguments.opaque(device, 256));
                CHECK(lock_device == 0);
                CHECK(std::string(device.begin(), device.end()) == "inst0");
                result.u32(0);
                result.u32(42);
                result.u32(abort_acceptor_.local_endpoint().port());
                result.u32(4);  // force write chunking
            } else if (call.procedure == wrvisa::vxi11::kDeviceWrite) {
                std::uint32_t link = 0;
                std::uint32_t io_timeout = 0;
                std::uint32_t lock_timeout = 0;
                std::uint32_t flags = 0;
                std::vector<std::uint8_t> data;
                CHECK(arguments.u32(link) && link == 42);
                CHECK(arguments.u32(io_timeout));
                CHECK(arguments.u32(lock_timeout));
                CHECK(arguments.u32(flags));
                CHECK(arguments.opaque(data, 1024));
                command_.append(data.begin(), data.end());
                if ((flags & wrvisa::vxi11::kFlagEnd) != 0) {
                    if (command_ == "*IDN?\n") {
                        response_ = "VXI-OK\n";
                    } else if (command_ == "SPLIT\n") {
                        response_ = "ABCD\n";
                    } else if (command_ == "HOLD\n" ||
                               command_ == "TIMEOUT\n") {
                        response_ = "__HOLD__";
                    } else if (command_ == "AGAIN\n") {
                        response_ = "AGAIN-OK\n";
                    } else if (command_ == "AFTER\n") {
                        response_ = "AFTER-OK\n";
                    } else if (command_ == "MALFORMED\n") {
                        response_ = "AB";
                        malformed_phase_ = 1;
                    } else {
                        response_ = command_;
                    }
                    command_.clear();
                }
                result.u32(0);
                result.u32(static_cast<std::uint32_t>(data.size()));
            } else if (call.procedure == wrvisa::vxi11::kDeviceRead) {
                std::uint32_t link = 0;
                std::uint32_t requested = 0;
                std::uint32_t ignored = 0;
                CHECK(arguments.u32(link) && link == 42);
                CHECK(arguments.u32(requested));
                CHECK(arguments.u32(ignored));
                CHECK(arguments.u32(ignored));
                CHECK(arguments.u32(ignored));
                CHECK(arguments.u32(ignored));
                if (malformed_phase_ == 2) {
                    malformed_phase_ = 3;
                    result.u32(0);  // Deliberately omit reason and data.
                    write_reply(core_socket_, call, result);
                    continue;
                }
                if (malformed_phase_ == 3) {
                    malformed_phase_ = 0;
                    result.u32(0);
                    result.u32(wrvisa::vxi11::kReasonEnd);
                    const std::string tail = "CD\n";
                    result.opaque(std::span<const std::uint8_t>(
                        reinterpret_cast<const std::uint8_t*>(tail.data()),
                        tail.size()));
                    write_reply(core_socket_, call, result);
                    continue;
                }
                if (response_ == "__HOLD__") {
                    std::unique_lock lock(abort_mutex_);
                    abort_condition_.wait(lock, [this] { return aborted_; });
                    aborted_ = false;
                    response_.clear();
                    result.u32(23);
                    result.u32(0);
                    result.opaque({});
                    write_reply(core_socket_, call, result);
                    continue;
                }
                const auto amount = std::min<std::size_t>(requested, response_.size());
                const bool end = amount == response_.size() &&
                                 malformed_phase_ == 0;
                std::vector<std::uint8_t> data(response_.begin(),
                                               response_.begin() +
                                                   static_cast<std::ptrdiff_t>(amount));
                response_.erase(0, amount);
                if (malformed_phase_ == 1) {
                    malformed_phase_ = 2;
                }
                result.u32(0);
                result.u32(end ? wrvisa::vxi11::kReasonEnd
                               : wrvisa::vxi11::kReasonRequestedCount);
                result.opaque(data);
            } else if (call.procedure == wrvisa::vxi11::kDeviceReadStb) {
                result.u32(0);
                result.u32(0x24);
            } else if (call.procedure == wrvisa::vxi11::kDeviceTrigger) {
                triggered_ = true;
                result.u32(0);
            } else if (call.procedure == wrvisa::vxi11::kDeviceClear) {
                command_.clear();
                response_.clear();
                result.u32(0);
            } else if (call.procedure == wrvisa::vxi11::kDeviceLock) {
                CHECK(!locked_);
                locked_ = true;
                result.u32(0);
            } else if (call.procedure == wrvisa::vxi11::kDeviceUnlock) {
                CHECK(locked_);
                locked_ = false;
                result.u32(0);
            } else if (call.procedure == wrvisa::vxi11::kDestroyLink) {
                result.u32(0);
                write_reply(core_socket_, call, result);
                return;
            } else {
                result.u32(8);
            }
            write_reply(core_socket_, call, result);
        }
    }

    asio::io_context context_;
    asio::ip::tcp::acceptor portmapper_acceptor_;
    asio::ip::tcp::acceptor core_acceptor_;
    asio::ip::tcp::acceptor abort_acceptor_;
    asio::ip::tcp::socket portmapper_socket_{context_};
    asio::ip::tcp::socket core_socket_{context_};
    asio::ip::tcp::socket abort_socket_{context_};
    std::thread portmapper_worker_;
    std::thread core_worker_;
    std::thread abort_worker_;
    std::string command_;
    std::string response_;
    bool triggered_{false};
    bool locked_{false};
    int malformed_phase_{0};
    std::mutex abort_mutex_;
    std::condition_variable abort_condition_;
    bool aborted_{false};
};

class MissingCoreServer final {
public:
    MissingCoreServer()
        : acceptor_(context_, asio::ip::tcp::endpoint(
                                  asio::ip::address_v4::loopback(), 0)) {
        worker_ = std::thread([this] {
            asio::ip::tcp::socket socket(context_);
            asio::error_code error;
            acceptor_.accept(socket, error);
            CHECK(!error);
            const auto call = read_call(socket);
            CHECK(call.procedure == wrvisa::vxi11::kPortmapperGetPort);
            wrvisa::vxi11::XdrWriter result;
            result.u32(0);
            write_reply(socket, call, result);
        });
    }

    ~MissingCoreServer() {
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

ViStatus write_text(ViSession session, const std::string& text) {
    ViUInt32 count = 0;
    const auto status = viWrite(session, reinterpret_cast<ViConstBuf>(text.data()),
                                static_cast<ViUInt32>(text.size()), &count);
    CHECK(status < VI_SUCCESS || count == text.size());
    return status;
}

}  // namespace

int main() {
    Vxi11Simulator simulator;
    ViSession rm = VI_NULL;
    CHECK(viOpenDefaultRM(&rm) == VI_SUCCESS);
    CHECK(wrvisaSetTcpipServicePort(rm, "127.0.0.1",
                                    WRVISA_TCPIP_PROTOCOL_VXI11,
                                    simulator.portmapper_port()) == VI_SUCCESS);
    ViSession session = VI_NULL;
    CHECK(viOpen(rm, "TCPIP0::127.0.0.1::inst0::INSTR", VI_NO_LOCK, 1000,
                 &session) == VI_SUCCESS);
    CHECK(session != VI_NULL);

    ViUInt16 port = 0;
    CHECK(viGetAttribute(session, VI_ATTR_TCPIP_PORT, &port) == VI_SUCCESS);
    CHECK(port == simulator.core_port());
    CHECK(viSetAttribute(session, VI_ATTR_TMO_VALUE, 1000) == VI_SUCCESS);
    CHECK(viSetAttribute(session, VI_ATTR_TERMCHAR, '\n') == VI_SUCCESS);
    CHECK(viSetAttribute(session, VI_ATTR_TERMCHAR_EN, VI_TRUE) == VI_SUCCESS);

    CHECK(write_text(session, "*IDN?\n") == VI_SUCCESS);
    std::array<ViByte, 64> buffer{};
    ViUInt32 count = 0;
    CHECK(viRead(session, buffer.data(), buffer.size(), &count) ==
          VI_SUCCESS_TERM_CHAR);
    CHECK(std::string(reinterpret_cast<char*>(buffer.data()), count) == "VXI-OK\n");

    CHECK(write_text(session, "SPLIT\n") == VI_SUCCESS);
    CHECK(viRead(session, buffer.data(), 2, &count) == VI_SUCCESS_MAX_CNT);
    CHECK(std::string(reinterpret_cast<char*>(buffer.data()), count) == "AB");
    CHECK(viRead(session, buffer.data(), buffer.size(), &count) ==
          VI_SUCCESS_TERM_CHAR);
    CHECK(std::string(reinterpret_cast<char*>(buffer.data()), count) == "CD\n");

    CHECK(write_text(session, "MALFORMED\n") == VI_SUCCESS);
    count = 99;
    CHECK(viRead(session, buffer.data(), buffer.size(), &count) == VI_ERROR_IO);
    CHECK(count == 0);
    CHECK(viRead(session, buffer.data(), buffer.size(), &count) ==
          VI_SUCCESS_TERM_CHAR);
    CHECK(std::string(reinterpret_cast<char*>(buffer.data()), count) ==
          "ABCD\n");

    ViUInt16 status_byte = 0;
    CHECK(viReadSTB(session, &status_byte) == VI_SUCCESS);
    CHECK(status_byte == 0x24);
    CHECK(viAssertTrigger(session, VI_TRIG_PROT_DEFAULT) == VI_SUCCESS);
    CHECK(viLock(session, VI_EXCLUSIVE_LOCK, 1000, nullptr, nullptr) == VI_SUCCESS);
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
    CHECK(blocked.get() == VI_ERROR_ABORT);
    CHECK(blocked_count == 0);
    CHECK(viSetAttribute(session, VI_ATTR_TMO_VALUE, 1000) == VI_SUCCESS);
    CHECK(write_text(session, "AGAIN\n") == VI_SUCCESS);
    CHECK(viRead(session, buffer.data(), buffer.size(), &count) ==
          VI_SUCCESS_TERM_CHAR);
    CHECK(std::string(reinterpret_cast<char*>(buffer.data()), count) ==
          "AGAIN-OK\n");
    CHECK(write_text(session, "TIMEOUT\n") == VI_SUCCESS);
    CHECK(viSetAttribute(session, VI_ATTR_TMO_VALUE, 30) == VI_SUCCESS);
    count = 99;
    CHECK(viRead(session, buffer.data(), buffer.size(), &count) == VI_ERROR_TMO);
    CHECK(count == 0);
    CHECK(viSetAttribute(session, VI_ATTR_TMO_VALUE, 1000) == VI_SUCCESS);
    CHECK(write_text(session, "AFTER\n") == VI_SUCCESS);
    CHECK(viRead(session, buffer.data(), buffer.size(), &count) ==
          VI_SUCCESS_TERM_CHAR);
    CHECK(std::string(reinterpret_cast<char*>(buffer.data()), count) ==
          "AFTER-OK\n");
    CHECK(simulator.triggered());
    CHECK(viClose(session) == VI_SUCCESS);
    CHECK(viClose(rm) == VI_SUCCESS);

    MissingCoreServer missing;
    CHECK(viOpenDefaultRM(&rm) == VI_SUCCESS);
    CHECK(wrvisaSetTcpipServicePort(rm, "127.0.0.1",
                                    WRVISA_TCPIP_PROTOCOL_VXI11,
                                    missing.port()) == VI_SUCCESS);
    session = 123;
    CHECK(viOpen(rm, "TCPIP0::127.0.0.1::inst0::INSTR", VI_NO_LOCK, 1000,
                 &session) == VI_ERROR_RSRC_NFOUND);
    CHECK(session == VI_NULL);
    CHECK(viClose(rm) == VI_SUCCESS);
    return 0;
}
