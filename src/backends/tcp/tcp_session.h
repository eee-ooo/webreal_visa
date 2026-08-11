#ifndef WRVISA_BACKENDS_TCP_TCP_SESSION_H
#define WRVISA_BACKENDS_TCP_TCP_SESSION_H

#include <memory>
#include <string>

#include <asio.hpp>

#include "backends/asio/async_stream.h"

namespace wrvisa {

class TcpBackendSession final
    : public AsyncStreamBackend<asio::ip::tcp::socket> {
public:
    static std::unique_ptr<TcpBackendSession> create(const std::string& host,
                                                     ViUInt16 port,
                                                     ViUInt32 timeout,
                                                     ViStatus& status);

    ViStatus clear() override;
    ViStatus set_attribute(ViAttr attribute, ViAttrState value) override;
    ViStatus get_attribute(ViAttr attribute, void* value) override;

private:
    TcpBackendSession(std::string host, ViUInt16 port)
        : host_(std::move(host)), port_(port) {}

    ViStatus connect(ViUInt32 timeout);

    std::string host_;
    std::string address_;
    ViUInt16 port_{0};
    ViBoolean no_delay_{VI_FALSE};
    ViBoolean keep_alive_{VI_FALSE};
};

}  // namespace wrvisa

#endif
