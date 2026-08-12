#ifndef WRVISA_BACKENDS_ASIO_REQUEST_CHANNEL_H
#define WRVISA_BACKENDS_ASIO_REQUEST_CHANNEL_H

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "runtime/operation.h"

namespace wrvisa {

enum class ResponseFraming {
    none,
    rpc_record,
    hislip_frame,
};

class RequestChannel final {
public:
    explicit RequestChannel(std::size_t maximum_frame_size = 16u * 1024u * 1024u,
                            bool drain_on_cancel = false);
    ~RequestChannel();

    RequestChannel(const RequestChannel&) = delete;
    RequestChannel& operator=(const RequestChannel&) = delete;

    ViStatus connect(const std::string& host, std::uint16_t port, ViUInt32 timeout,
                     std::string& address);
    ViStatus exchange(Operation& operation, std::vector<std::uint8_t> request,
                      ResponseFraming framing, std::vector<std::uint8_t>& response);
    void set_cancel_observer(std::function<void()> observer);
    void close() noexcept;

private:
    struct State;
    std::shared_ptr<State> state_;
};

}  // namespace wrvisa

#endif
