#include "backends/serial/serial_session.h"

#include <limits>

#if defined(_WIN32)
#include <windows.h>
#else
#include <termios.h>
#endif

#include "platform/serial_path.h"

namespace wrvisa {
namespace {

ViStatus map_open_error(const asio::error_code& error) noexcept {
    return error == asio::error::access_denied ? VI_ERROR_NPERMISSION
                                                : VI_ERROR_RSRC_NFOUND;
}

}  // namespace

std::unique_ptr<SerialBackendSession> SerialBackendSession::create(
    const std::string& native_path, ViStatus& status) {
    auto session = std::unique_ptr<SerialBackendSession>(
        new SerialBackendSession(native_path));
    status = session->open();
    if (status < VI_SUCCESS) {
        return nullptr;
    }
    return session;
}

ViStatus SerialBackendSession::open() {
    return on_stream([this] {
        asio::error_code error;
        stream().open(platform_serial_path(native_path_), error);
        if (error) {
            return map_open_error(error);
        }
        stream().set_option(asio::serial_port_base::baud_rate(baud_), error);
        if (!error) {
            stream().set_option(asio::serial_port_base::character_size(data_bits_), error);
        }
        if (!error) {
            stream().set_option(asio::serial_port_base::parity(
                                    asio::serial_port_base::parity::none),
                                error);
        }
        if (!error) {
            stream().set_option(asio::serial_port_base::stop_bits(
                                    asio::serial_port_base::stop_bits::one),
                                error);
        }
        if (!error) {
            stream().set_option(asio::serial_port_base::flow_control(
                                    asio::serial_port_base::flow_control::none),
                                error);
        }
        if (error) {
            asio::error_code ignored;
            stream().close(ignored);
            return VI_ERROR_IO;
        }
        return VI_SUCCESS;
    });
}

ViStatus SerialBackendSession::clear() {
    return on_stream([this] {
#if defined(_WIN32)
        return PurgeComm(stream().native_handle(), PURGE_RXCLEAR | PURGE_TXCLEAR) != 0
                   ? VI_SUCCESS
                   : VI_ERROR_IO;
#else
        return tcflush(stream().native_handle(), TCIOFLUSH) == 0 ? VI_SUCCESS
                                                                 : VI_ERROR_IO;
#endif
    });
}

ViStatus SerialBackendSession::flush(ViUInt16 mask) {
    const auto buffered = AsyncStreamBackend<asio::serial_port>::flush(mask);
    if (buffered < VI_SUCCESS) {
        return buffered;
    }
    const bool discard_input =
        (mask & (VI_READ_BUF_DISCARD | VI_IO_IN_BUF_DISCARD)) != 0;
    const bool discard_output =
        (mask & (VI_WRITE_BUF_DISCARD | VI_IO_OUT_BUF_DISCARD)) != 0;
    if (!discard_input && !discard_output) {
        return VI_SUCCESS;
    }
    return on_stream([this, discard_input, discard_output] {
#if defined(_WIN32)
        DWORD flags = 0;
        if (discard_input) {
            flags |= PURGE_RXCLEAR;
        }
        if (discard_output) {
            flags |= PURGE_TXCLEAR;
        }
        return PurgeComm(stream().native_handle(), flags) != 0 ? VI_SUCCESS
                                                                : VI_ERROR_IO;
#else
        const int queue = discard_input && discard_output
                              ? TCIOFLUSH
                              : (discard_input ? TCIFLUSH : TCOFLUSH);
        return tcflush(stream().native_handle(), queue) == 0 ? VI_SUCCESS
                                                              : VI_ERROR_IO;
#endif
    });
}

ViStatus SerialBackendSession::set_attribute(ViAttr attribute, ViAttrState value) {
    switch (attribute) {
        case VI_ATTR_ASRL_BAUD:
            if (value == 0 || value > std::numeric_limits<unsigned int>::max()) {
                return VI_ERROR_NSUP_ATTR_STATE;
            }
            return on_stream([this, value] {
                asio::error_code error;
                stream().set_option(asio::serial_port_base::baud_rate(
                                        static_cast<unsigned int>(value)),
                                    error);
                if (!error) {
                    baud_ = static_cast<ViUInt32>(value);
                }
                return error ? VI_ERROR_NSUP_ATTR_STATE : VI_SUCCESS;
            });
        case VI_ATTR_ASRL_DATA_BITS:
            if (value < 5 || value > 8) {
                return VI_ERROR_NSUP_ATTR_STATE;
            }
            return on_stream([this, value] {
                asio::error_code error;
                stream().set_option(asio::serial_port_base::character_size(
                                        static_cast<unsigned int>(value)),
                                    error);
                if (!error) {
                    data_bits_ = static_cast<ViUInt16>(value);
                }
                return error ? VI_ERROR_NSUP_ATTR_STATE : VI_SUCCESS;
            });
        case VI_ATTR_ASRL_PARITY: {
            asio::serial_port_base::parity::type option;
            if (value == VI_ASRL_PAR_NONE) {
                option = asio::serial_port_base::parity::none;
            } else if (value == VI_ASRL_PAR_ODD) {
                option = asio::serial_port_base::parity::odd;
            } else if (value == VI_ASRL_PAR_EVEN) {
                option = asio::serial_port_base::parity::even;
            } else {
                return VI_ERROR_NSUP_ATTR_STATE;
            }
            return on_stream([this, value, option] {
                asio::error_code error;
                stream().set_option(asio::serial_port_base::parity(option), error);
                if (!error) {
                    parity_ = static_cast<ViUInt16>(value);
                }
                return error ? VI_ERROR_NSUP_ATTR_STATE : VI_SUCCESS;
            });
        }
        case VI_ATTR_ASRL_STOP_BITS: {
            asio::serial_port_base::stop_bits::type option;
            if (value == VI_ASRL_STOP_ONE) {
                option = asio::serial_port_base::stop_bits::one;
            } else if (value == VI_ASRL_STOP_ONE5) {
                option = asio::serial_port_base::stop_bits::onepointfive;
            } else if (value == VI_ASRL_STOP_TWO) {
                option = asio::serial_port_base::stop_bits::two;
            } else {
                return VI_ERROR_NSUP_ATTR_STATE;
            }
            return on_stream([this, value, option] {
                asio::error_code error;
                stream().set_option(asio::serial_port_base::stop_bits(option), error);
                if (!error) {
                    stop_bits_ = static_cast<ViUInt16>(value);
                }
                return error ? VI_ERROR_NSUP_ATTR_STATE : VI_SUCCESS;
            });
        }
        case VI_ATTR_ASRL_FLOW_CNTRL: {
            asio::serial_port_base::flow_control::type option;
            if (value == VI_ASRL_FLOW_NONE) {
                option = asio::serial_port_base::flow_control::none;
            } else if (value == VI_ASRL_FLOW_XON_XOFF) {
                option = asio::serial_port_base::flow_control::software;
            } else if (value == VI_ASRL_FLOW_RTS_CTS) {
                option = asio::serial_port_base::flow_control::hardware;
            } else {
                return VI_ERROR_NSUP_ATTR_STATE;
            }
            return on_stream([this, value, option] {
                asio::error_code error;
                stream().set_option(asio::serial_port_base::flow_control(option), error);
                if (!error) {
                    flow_control_ = static_cast<ViUInt16>(value);
                }
                return error ? VI_ERROR_NSUP_ATTR_STATE : VI_SUCCESS;
            });
        }
        default:
            return VI_ERROR_NSUP_ATTR;
    }
}

ViStatus SerialBackendSession::get_attribute(ViAttr attribute, void* value) {
    return on_stream([this, attribute, value] {
        switch (attribute) {
            case VI_ATTR_ASRL_BAUD:
                *static_cast<ViUInt32*>(value) = baud_;
                return VI_SUCCESS;
            case VI_ATTR_ASRL_DATA_BITS:
                *static_cast<ViUInt16*>(value) = data_bits_;
                return VI_SUCCESS;
            case VI_ATTR_ASRL_PARITY:
                *static_cast<ViUInt16*>(value) = parity_;
                return VI_SUCCESS;
            case VI_ATTR_ASRL_STOP_BITS:
                *static_cast<ViUInt16*>(value) = stop_bits_;
                return VI_SUCCESS;
            case VI_ATTR_ASRL_FLOW_CNTRL:
                *static_cast<ViUInt16*>(value) = flow_control_;
                return VI_SUCCESS;
            default:
                return VI_ERROR_NSUP_ATTR;
        }
    });
}

}  // namespace wrvisa
