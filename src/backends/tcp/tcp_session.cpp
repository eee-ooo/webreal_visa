#include "backends/tcp/tcp_session.h"

#include <cstring>
#include <future>
#include <memory>
#include <string_view>

namespace wrvisa {
namespace {

struct ConnectState {
    explicit ConnectState(asio::any_io_executor executor)
        : resolver(executor), timer(executor), socket(executor) {}

    asio::ip::tcp::resolver resolver;
    asio::steady_timer timer;
    asio::ip::tcp::socket socket;
    std::promise<ViStatus> completion;
    std::string address;
    bool done{false};
};

ViStatus map_connect_error(const asio::error_code& error) noexcept {
    if (error == asio::error::access_denied) {
        return VI_ERROR_NPERMISSION;
    }
    if (error == asio::error::timed_out) {
        return VI_ERROR_TMO;
    }
    return VI_ERROR_RSRC_NFOUND;
}

void finish_connect(const std::shared_ptr<ConnectState>& state, ViStatus status) {
    if (state->done) {
        return;
    }
    state->done = true;
    state->timer.cancel();
    state->resolver.cancel();
    if (status < VI_SUCCESS) {
        asio::error_code ignored;
        state->socket.cancel(ignored);
        state->socket.close(ignored);
    }
    state->completion.set_value(status);
}

void copy_string(void* destination, std::string_view value) {
    const auto amount = std::min<std::size_t>(value.size(), VI_FIND_BUFLEN - 1u);
    std::memcpy(destination, value.data(), amount);
    static_cast<ViChar*>(destination)[amount] = '\0';
}

}  // namespace

std::unique_ptr<TcpBackendSession> TcpBackendSession::create(
    const std::string& host, ViUInt16 port, ViUInt32 timeout, ViStatus& status) {
    auto session = std::unique_ptr<TcpBackendSession>(
        new TcpBackendSession(host, port));
    status = session->connect(timeout);
    if (status < VI_SUCCESS) {
        return nullptr;
    }
    return session;
}

ViStatus TcpBackendSession::connect(ViUInt32 timeout) {
    auto state = std::make_shared<ConnectState>(executor());
    auto future = state->completion.get_future();
    const auto host = host_;
    const auto service = std::to_string(port_);
    asio::post(executor(), [state, timeout, host, service] {
        if (timeout != VI_TMO_INFINITE) {
            state->timer.expires_after(std::chrono::milliseconds(timeout));
            state->timer.async_wait([state](const asio::error_code& error) {
                if (!error) {
                    finish_connect(state, VI_ERROR_TMO);
                }
            });
        }
        state->resolver.async_resolve(
            host, service,
            [state](const asio::error_code& error,
                    const asio::ip::tcp::resolver::results_type& endpoints) {
                if (error) {
                    finish_connect(state, map_connect_error(error));
                    return;
                }
                asio::async_connect(
                    state->socket, endpoints,
                    [state](const asio::error_code& connect_error,
                            const asio::ip::tcp::endpoint& endpoint) {
                        if (!connect_error) {
                            state->address = endpoint.address().to_string();
                        }
                        finish_connect(state,
                                       connect_error ? map_connect_error(connect_error)
                                                     : VI_SUCCESS);
                    });
            });
    });
    const auto status = future.get();
    if (status < VI_SUCCESS) {
        return status;
    }
    return on_stream([this, state] {
        stream() = std::move(state->socket);
        address_ = std::move(state->address);
        return VI_SUCCESS;
    });
}

ViStatus TcpBackendSession::clear() { return VI_ERROR_NSUP_OPER; }

ViStatus TcpBackendSession::set_attribute(ViAttr attribute, ViAttrState value) {
    if (attribute == VI_ATTR_TCPIP_ADDR || attribute == VI_ATTR_TCPIP_HOSTNAME ||
        attribute == VI_ATTR_TCPIP_PORT) {
        return VI_ERROR_ATTR_READONLY;
    }
    if (attribute != VI_ATTR_TCPIP_NODELAY && attribute != VI_ATTR_TCPIP_KEEPALIVE) {
        return VI_ERROR_NSUP_ATTR;
    }
    if (value != VI_FALSE && value != VI_TRUE) {
        return VI_ERROR_NSUP_ATTR_STATE;
    }
    return on_stream([this, attribute, value] {
        asio::error_code error;
        if (attribute == VI_ATTR_TCPIP_NODELAY) {
            stream().set_option(asio::ip::tcp::no_delay(value == VI_TRUE), error);
            if (!error) {
                no_delay_ = static_cast<ViBoolean>(value);
            }
        } else {
            stream().set_option(asio::socket_base::keep_alive(value == VI_TRUE), error);
            if (!error) {
                keep_alive_ = static_cast<ViBoolean>(value);
            }
        }
        return error ? VI_ERROR_IO : VI_SUCCESS;
    });
}

ViStatus TcpBackendSession::get_attribute(ViAttr attribute, void* value) {
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
        case VI_ATTR_TCPIP_NODELAY:
            return on_stream([this, value] {
                *static_cast<ViBoolean*>(value) = no_delay_;
                return VI_SUCCESS;
            });
        case VI_ATTR_TCPIP_KEEPALIVE:
            return on_stream([this, value] {
                *static_cast<ViBoolean*>(value) = keep_alive_;
                return VI_SUCCESS;
            });
        default:
            return VI_ERROR_NSUP_ATTR;
    }
}

}  // namespace wrvisa
