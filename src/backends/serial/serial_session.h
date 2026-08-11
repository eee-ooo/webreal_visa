#ifndef WRVISA_BACKENDS_SERIAL_SERIAL_SESSION_H
#define WRVISA_BACKENDS_SERIAL_SERIAL_SESSION_H

#include <memory>
#include <string>

#include <asio.hpp>

#include "backends/asio/async_stream.h"

namespace wrvisa {

class SerialBackendSession final
    : public AsyncStreamBackend<asio::serial_port> {
public:
    static std::unique_ptr<SerialBackendSession> create(const std::string& native_path,
                                                        ViStatus& status);

    ViStatus clear() override;
    ViStatus flush(ViUInt16 mask) override;
    ViStatus set_attribute(ViAttr attribute, ViAttrState value) override;
    ViStatus get_attribute(ViAttr attribute, void* value) override;

private:
    explicit SerialBackendSession(std::string native_path)
        : native_path_(std::move(native_path)) {}

    ViStatus open();

    std::string native_path_;
    ViUInt32 baud_{9600};
    ViUInt16 data_bits_{8};
    ViUInt16 parity_{VI_ASRL_PAR_NONE};
    ViUInt16 stop_bits_{VI_ASRL_STOP_ONE};
    ViUInt16 flow_control_{VI_ASRL_FLOW_NONE};
};

}  // namespace wrvisa

#endif
