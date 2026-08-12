#include "visa.h"

#include <cstring>
#include <cctype>
#include <exception>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "backends/mock/mock_session.h"
#include "backends/hislip/hislip_session.h"
#include "backends/serial/serial_session.h"
#include "backends/tcp/tcp_session.h"
#include "backends/vxi11/vxi11_session.h"
#include "core/handle_table.h"
#include "core/lock_manager.h"
#include "core/objects.h"
#include "resource/find_expression.h"
#include "resource/resource_parser.h"
#include "webreal_visa_ext.h"

namespace wrvisa {
namespace {

template <typename Function>
ViStatus api_guard(Function&& function) noexcept {
    try {
        return function();
    } catch (const std::bad_alloc&) {
        return VI_ERROR_ALLOC;
    } catch (...) {
        return VI_ERROR_SYSTEM_ERROR;
    }
}

void copy_output(ViChar* destination, std::string_view value) {
    const auto count = value.size() < VI_FIND_BUFLEN - 1u
                           ? value.size()
                           : static_cast<std::size_t>(VI_FIND_BUFLEN - 1u);
    std::memcpy(destination, value.data(), count);
    destination[count] = '\0';
}

ViStatus register_child(const std::shared_ptr<ResourceManager>& manager,
                        const std::shared_ptr<Object>& child, ObjectType type,
                        ViObject& handle) {
    handle = handles().insert(type, child);
    if (manager->add_child(handle)) {
        return VI_SUCCESS;
    }
    static_cast<void>(handles().remove(handle));
    child->close();
    handle = VI_NULL;
    return VI_ERROR_INV_OBJECT;
}

ViStatus require_outputs(std::initializer_list<const void*> outputs) {
    for (const auto output : outputs) {
        if (output == nullptr) {
            return VI_ERROR_INV_PARAMETER;
        }
    }
    return VI_SUCCESS;
}

const char* status_description(ViStatus status) {
    switch (status) {
        case VI_SUCCESS:
            return "Operation completed successfully.";
        case VI_SUCCESS_TERM_CHAR:
            return "The termination character was read.";
        case VI_SUCCESS_MAX_CNT:
            return "The requested byte count was read.";
        case VI_SUCCESS_NESTED_SHARED:
            return "The session already held a shared lock.";
        case VI_SUCCESS_NESTED_EXCLUSIVE:
            return "The session already held an exclusive lock.";
        case VI_ERROR_SYSTEM_ERROR:
            return "An internal system error occurred.";
        case VI_ERROR_INV_OBJECT:
            return "The session or object handle is invalid.";
        case VI_ERROR_RSRC_LOCKED:
            return "The resource is locked by another session.";
        case VI_ERROR_INV_EXPR:
            return "The resource expression is invalid or unsupported.";
        case VI_ERROR_RSRC_NFOUND:
            return "No resource matched the request.";
        case VI_ERROR_INV_RSRC_NAME:
            return "The resource name is invalid.";
        case VI_ERROR_INV_ACC_MODE:
            return "The access mode is invalid.";
        case VI_ERROR_TMO:
            return "The operation timed out.";
        case VI_ERROR_INV_DEGREE:
            return "The termination degree is invalid.";
        case VI_ERROR_INV_JOB_ID:
            return "The job identifier is invalid.";
        case VI_ERROR_NSUP_ATTR:
            return "The attribute is not supported.";
        case VI_ERROR_NSUP_ATTR_STATE:
            return "The attribute value is not supported.";
        case VI_ERROR_ATTR_READONLY:
            return "The attribute is read-only.";
        case VI_ERROR_INV_LOCK_TYPE:
            return "The lock type is invalid.";
        case VI_ERROR_INV_ACCESS_KEY:
            return "The lock access key is invalid.";
        case VI_ERROR_ABORT:
            return "The operation was aborted.";
        case VI_ERROR_ALLOC:
            return "Memory allocation failed.";
        case VI_ERROR_INV_MASK:
            return "The buffer mask is invalid.";
        case VI_ERROR_IO:
            return "An I/O error occurred.";
        case VI_ERROR_NSUP_OPER:
            return "The operation is not supported by this implementation or backend.";
        case VI_ERROR_RSRC_BUSY:
            return "The resource is busy.";
        case VI_ERROR_INV_PARAMETER:
            return "A parameter is invalid.";
        case VI_ERROR_INV_PROT:
            return "The protocol is invalid.";
        case VI_ERROR_INV_SIZE:
            return "The requested size is invalid.";
        case VI_ERROR_SESN_NLOCKED:
            return "The session does not own a lock.";
        case VI_ERROR_INTF_NUM_NCONFIG:
            return "The interface number has no platform resource mapping.";
        case VI_ERROR_CONN_LOST:
            return "The connection to the resource was lost.";
        case VI_ERROR_NPERMISSION:
            return "Permission to access the resource was denied.";
        default:
            return nullptr;
    }
}

bool valid_open_mode(ViAccessMode mode) {
    constexpr ViAccessMode kKnown = VI_EXCLUSIVE_LOCK | VI_SHARED_LOCK | VI_LOAD_CONFIG;
    if ((mode & ~kKnown) != 0) {
        return false;
    }
    return (mode & VI_EXCLUSIVE_LOCK) == 0 || (mode & VI_SHARED_LOCK) == 0;
}

bool mock_discovery_opted_in(std::string_view expression) {
    constexpr std::string_view expected = WRVISA_MOCK_FIND_EXPRESSION;
    if (const auto attributes = expression.find('{');
        attributes != std::string_view::npos) {
        expression = expression.substr(0, attributes);
    }
    if (expression.size() != expected.size()) {
        return false;
    }
    for (std::size_t index = 0; index < expression.size(); ++index) {
        const auto left = static_cast<unsigned char>(expression[index]);
        const auto right = static_cast<unsigned char>(expected[index]);
        if (std::toupper(left) != std::toupper(right)) {
            return false;
        }
    }
    return true;
}

}  // namespace
}  // namespace wrvisa

extern "C" {

ViStatus WRVISA_CALL viOpenDefaultRM(ViPSession vi) {
    return wrvisa::api_guard([&] {
        if (vi == nullptr) {
            return VI_ERROR_INV_PARAMETER;
        }
        auto manager = std::make_shared<wrvisa::ResourceManager>();
        *vi = wrvisa::handles().insert(wrvisa::ObjectType::resource_manager, manager);
        return VI_SUCCESS;
    });
}

ViStatus WRVISA_CALL viGetDefaultRM(ViPSession vi) { return viOpenDefaultRM(vi); }

ViStatus WRVISA_CALL viFindRsrc(ViSession sesn, ViConstString expr, ViPFindList vi,
                                ViPUInt32 retCnt, ViChar desc[]) {
    return wrvisa::api_guard([&] {
        if (expr == nullptr || desc == nullptr) {
            return VI_ERROR_INV_PARAMETER;
        }
        if (vi != nullptr) {
            *vi = VI_NULL;
        }
        if (retCnt != nullptr) {
            *retCnt = 0;
        }
        auto manager = wrvisa::get_handle<wrvisa::ResourceManager>(
            sesn, wrvisa::ObjectType::resource_manager);
        if (!manager) {
            return VI_ERROR_INV_OBJECT;
        }
        std::string parse_error;
        auto expression = wrvisa::FindExpression::compile(expr, parse_error);
        if (!expression) {
            return VI_ERROR_INV_EXPR;
        }
        std::vector<std::string> matches;
        for (const auto& resource : manager->discoverable_resources()) {
            const auto parsed = wrvisa::parse_resource(resource);
            if (parsed && expression->matches(*parsed)) {
                matches.push_back(resource);
            }
        }
        const auto mock = wrvisa::parse_resource(WRVISA_MOCK_RESOURCE);
        if (mock && wrvisa::mock_discovery_opted_in(expr) &&
            expression->matches(*mock)) {
            matches.emplace_back(WRVISA_MOCK_RESOURCE);
        }
        if (matches.empty()) {
            return VI_ERROR_RSRC_NFOUND;
        }
        if (retCnt != nullptr) {
            *retCnt = static_cast<ViUInt32>(matches.size());
        }
        if (vi != nullptr) {
            auto list = std::make_shared<wrvisa::FindListObject>(manager, sesn,
                                                                matches);
            ViObject handle = VI_NULL;
            const auto status = wrvisa::register_child(
                manager, list, wrvisa::ObjectType::find_list, handle);
            if (status < VI_SUCCESS) {
                return status;
            }
            list->set_handle(handle);
            *vi = handle;
        }
        wrvisa::copy_output(desc, matches.front());
        return VI_SUCCESS;
    });
}

ViStatus WRVISA_CALL viFindNext(ViFindList vi, ViChar desc[]) {
    return wrvisa::api_guard([&] {
        if (desc == nullptr) {
            return VI_ERROR_INV_PARAMETER;
        }
        auto list = wrvisa::get_handle<wrvisa::FindListObject>(
            vi, wrvisa::ObjectType::find_list);
        if (!list) {
            return VI_ERROR_INV_OBJECT;
        }
        std::string next;
        if (!list->next(next)) {
            return VI_ERROR_RSRC_NFOUND;
        }
        wrvisa::copy_output(desc, next);
        return VI_SUCCESS;
    });
}

ViStatus WRVISA_CALL viParseRsrc(ViSession rmSesn, ViConstRsrc rsrcName,
                                 ViPUInt16 intfType, ViPUInt16 intfNum) {
    return wrvisa::api_guard([&] {
        if (rsrcName == nullptr ||
            wrvisa::require_outputs({intfType, intfNum}) != VI_SUCCESS) {
            return VI_ERROR_INV_PARAMETER;
        }
        auto manager = wrvisa::get_handle<wrvisa::ResourceManager>(
            rmSesn, wrvisa::ObjectType::resource_manager);
        if (!manager) {
            return VI_ERROR_INV_OBJECT;
        }
        const auto resolved = manager->resolve_resource(rsrcName);
        if (!resolved) {
            return VI_ERROR_INV_RSRC_NAME;
        }
        *intfType = resolved->descriptor.interface_type;
        *intfNum = resolved->descriptor.interface_number;
        return VI_SUCCESS;
    });
}

ViStatus WRVISA_CALL viParseRsrcEx(ViSession rmSesn, ViConstRsrc rsrcName,
                                   ViPUInt16 intfType, ViPUInt16 intfNum,
                                   ViChar rsrcClass[], ViChar expandedUnaliasedName[],
                                   ViChar aliasIfExists[]) {
    return wrvisa::api_guard([&] {
        if (rsrcName == nullptr ||
            wrvisa::require_outputs({intfType, intfNum}) != VI_SUCCESS) {
            return VI_ERROR_INV_PARAMETER;
        }
        auto manager = wrvisa::get_handle<wrvisa::ResourceManager>(
            rmSesn, wrvisa::ObjectType::resource_manager);
        if (!manager) {
            return VI_ERROR_INV_OBJECT;
        }
        const auto resolved = manager->resolve_resource(rsrcName);
        if (!resolved) {
            return VI_ERROR_INV_RSRC_NAME;
        }
        *intfType = resolved->descriptor.interface_type;
        *intfNum = resolved->descriptor.interface_number;
        if (rsrcClass != nullptr) {
            wrvisa::copy_output(rsrcClass, resolved->descriptor.resource_class);
        }
        if (expandedUnaliasedName != nullptr) {
            wrvisa::copy_output(expandedUnaliasedName,
                                resolved->descriptor.canonical_name);
        }
        if (aliasIfExists != nullptr) {
            wrvisa::copy_output(aliasIfExists, resolved->alias);
        }
        return VI_SUCCESS;
    });
}

ViStatus WRVISA_CALL viOpen(ViSession sesn, ViConstRsrc name, ViAccessMode mode,
                            ViUInt32 timeout, ViPSession vi) {
    return wrvisa::api_guard([&] {
        if (name == nullptr || vi == nullptr) {
            return VI_ERROR_INV_PARAMETER;
        }
        *vi = VI_NULL;
        if (!wrvisa::valid_open_mode(mode)) {
            return VI_ERROR_INV_ACC_MODE;
        }
        auto manager = wrvisa::get_handle<wrvisa::ResourceManager>(
            sesn, wrvisa::ObjectType::resource_manager);
        if (!manager) {
            return VI_ERROR_INV_OBJECT;
        }
        auto resolved = manager->resolve_resource(name);
        if (!resolved) {
            return VI_ERROR_INV_RSRC_NAME;
        }
        auto parsed = std::move(resolved->descriptor);
        std::unique_ptr<wrvisa::BackendSession> backend;
        ViStatus open_status = VI_SUCCESS;
        switch (parsed.kind) {
            case wrvisa::ResourceKind::project_mock:
                backend = std::make_unique<wrvisa::MockBackendSession>();
                break;
            case wrvisa::ResourceKind::tcpip_socket:
                backend = wrvisa::TcpBackendSession::create(
                    parsed.host, parsed.port, timeout, open_status);
                break;
            case wrvisa::ResourceKind::tcpip_instr:
                if (parsed.tcpip_protocol == wrvisa::TcpipProtocol::hislip) {
                    const auto port = manager->tcpip_service_port(
                        parsed.host, parsed.tcpip_protocol).value_or(4880);
                    backend = wrvisa::HiSlipBackendSession::create(
                        parsed.host, parsed.device_name, timeout, open_status, port);
                } else if (parsed.tcpip_protocol ==
                           wrvisa::TcpipProtocol::vxi11) {
                    const auto port = manager->tcpip_service_port(
                        parsed.host, parsed.tcpip_protocol).value_or(111);
                    backend = wrvisa::Vxi11BackendSession::create(
                        parsed.host, parsed.device_name, timeout, open_status, port);
                } else {
                    return VI_ERROR_NSUP_OPER;
                }
                break;
            case wrvisa::ResourceKind::asrl_instr: {
                const auto path = manager->serial_path(parsed.interface_number);
                if (!path) {
                    return VI_ERROR_INTF_NUM_NCONFIG;
                }
                backend = wrvisa::SerialBackendSession::create(*path, open_status);
                break;
            }
            default:
                return VI_ERROR_NSUP_OPER;
        }
        if (!backend) {
            return open_status;
        }
        auto session = std::make_shared<wrvisa::SessionObject>(
            manager, sesn, std::move(parsed),
            std::move(backend));
        ViObject handle = VI_NULL;
        auto status = wrvisa::register_child(
            manager, session, wrvisa::ObjectType::session, handle);
        if (status < VI_SUCCESS) {
            return status;
        }
        session->set_handle(handle);
        const auto lock_type = mode & (VI_EXCLUSIVE_LOCK | VI_SHARED_LOCK);
        if (lock_type != VI_NO_LOCK) {
            std::string access_key;
            status = session->lock(lock_type, timeout, nullptr, access_key);
            if (status < VI_SUCCESS) {
                auto removed = wrvisa::handles().remove(handle);
                if (removed) {
                    removed->close();
                }
                return status;
            }
        }
        *vi = handle;
        return status;
    });
}

ViStatus WRVISA_CALL viClose(ViObject vi) {
    return wrvisa::api_guard([&] {
        auto object = wrvisa::handles().remove(vi);
        if (!object) {
            return VI_ERROR_INV_OBJECT;
        }
        wrvisa::close_handle_object(vi, object);
        return VI_SUCCESS;
    });
}

ViStatus WRVISA_CALL viSetAttribute(ViObject vi, ViAttr attrName,
                                    ViAttrState attrValue) {
    return wrvisa::api_guard([&] {
        auto session = wrvisa::get_handle<wrvisa::SessionObject>(
            vi, wrvisa::ObjectType::session);
        return session ? session->set_attribute(attrName, attrValue)
                       : VI_ERROR_INV_OBJECT;
    });
}

ViStatus WRVISA_CALL viGetAttribute(ViObject vi, ViAttr attrName, void* attrValue) {
    return wrvisa::api_guard([&] {
        if (attrValue == nullptr) {
            return VI_ERROR_INV_PARAMETER;
        }
        if (auto session = wrvisa::get_handle<wrvisa::SessionObject>(
                vi, wrvisa::ObjectType::session)) {
            return session->get_attribute(attrName, attrValue);
        }
        if (wrvisa::get_handle<wrvisa::ResourceManager>(
                vi, wrvisa::ObjectType::resource_manager)) {
            switch (attrName) {
                case VI_ATTR_RSRC_CLASS:
                    wrvisa::copy_output(static_cast<ViChar*>(attrValue), "RM");
                    return VI_SUCCESS;
                case VI_ATTR_RSRC_IMPL_VERSION:
                    *static_cast<ViVersion*>(attrValue) = UINT32_C(0x00000400);
                    return VI_SUCCESS;
                case VI_ATTR_RSRC_SPEC_VERSION:
                    *static_cast<ViVersion*>(attrValue) = VI_SPEC_VERSION;
                    return VI_SUCCESS;
                default:
                    return VI_ERROR_NSUP_ATTR;
            }
        }
        return VI_ERROR_INV_OBJECT;
    });
}

ViStatus WRVISA_CALL viStatusDesc(ViObject vi, ViStatus status, ViChar desc[]) {
    return wrvisa::api_guard([&] {
        if (desc == nullptr) {
            return VI_ERROR_INV_PARAMETER;
        }
        if (!wrvisa::handles().get_any(vi)) {
            return VI_ERROR_INV_OBJECT;
        }
        const auto* description = wrvisa::status_description(status);
        if (description == nullptr) {
            return VI_ERROR_NSUP_OPER;
        }
        wrvisa::copy_output(desc, description);
        return VI_SUCCESS;
    });
}

ViStatus WRVISA_CALL viTerminate(ViObject vi, ViUInt16 degree, ViJobId jobId) {
    return wrvisa::api_guard([&] {
        if (degree != VI_NULL) {
            return VI_ERROR_INV_DEGREE;
        }
        if (jobId != VI_NULL) {
            return VI_ERROR_INV_JOB_ID;
        }
        auto session = wrvisa::get_handle<wrvisa::SessionObject>(
            vi, wrvisa::ObjectType::session);
        return session ? session->cancel_operations() : VI_ERROR_INV_OBJECT;
    });
}

ViStatus WRVISA_CALL viLock(ViSession vi, ViAccessMode lockType, ViUInt32 timeout,
                            ViConstKeyId requestedKey, ViChar accessKey[]) {
    return wrvisa::api_guard([&] {
        auto session = wrvisa::get_handle<wrvisa::SessionObject>(
            vi, wrvisa::ObjectType::session);
        if (!session) {
            return VI_ERROR_INV_OBJECT;
        }
        std::string key;
        const auto status = session->lock(lockType, timeout, requestedKey, key);
        if (status >= VI_SUCCESS && accessKey != nullptr) {
            wrvisa::copy_output(accessKey, key);
        }
        return status;
    });
}

ViStatus WRVISA_CALL viUnlock(ViSession vi) {
    return wrvisa::api_guard([&] {
        auto session = wrvisa::get_handle<wrvisa::SessionObject>(
            vi, wrvisa::ObjectType::session);
        if (!session) {
            return VI_ERROR_INV_OBJECT;
        }
        return session->unlock();
    });
}

ViStatus WRVISA_CALL viRead(ViSession vi, ViPBuf buf, ViUInt32 cnt,
                            ViPUInt32 retCnt) {
    return wrvisa::api_guard([&] {
        if (buf == nullptr || retCnt == nullptr || cnt == 0) {
            return VI_ERROR_INV_PARAMETER;
        }
        auto session = wrvisa::get_handle<wrvisa::SessionObject>(
            vi, wrvisa::ObjectType::session);
        return session ? session->read(buf, cnt, retCnt) : VI_ERROR_INV_OBJECT;
    });
}

ViStatus WRVISA_CALL viWrite(ViSession vi, ViConstBuf buf, ViUInt32 cnt,
                             ViPUInt32 retCnt) {
    return wrvisa::api_guard([&] {
        if (buf == nullptr || retCnt == nullptr || cnt == 0) {
            return VI_ERROR_INV_PARAMETER;
        }
        auto session = wrvisa::get_handle<wrvisa::SessionObject>(
            vi, wrvisa::ObjectType::session);
        return session ? session->write(buf, cnt, retCnt) : VI_ERROR_INV_OBJECT;
    });
}

ViStatus WRVISA_CALL viAssertTrigger(ViSession vi, ViUInt16 protocol) {
    return wrvisa::api_guard([&] {
        auto session = wrvisa::get_handle<wrvisa::SessionObject>(
            vi, wrvisa::ObjectType::session);
        if (!session) {
            return VI_ERROR_INV_OBJECT;
        }
        return session->assert_trigger(protocol);
    });
}

ViStatus WRVISA_CALL viReadSTB(ViSession vi, ViPUInt16 status) {
    return wrvisa::api_guard([&] {
        auto session = wrvisa::get_handle<wrvisa::SessionObject>(
            vi, wrvisa::ObjectType::session);
        return session ? session->read_stb(status) : VI_ERROR_INV_OBJECT;
    });
}

ViStatus WRVISA_CALL viClear(ViSession vi) {
    return wrvisa::api_guard([&] {
        auto session = wrvisa::get_handle<wrvisa::SessionObject>(
            vi, wrvisa::ObjectType::session);
        return session ? session->clear() : VI_ERROR_INV_OBJECT;
    });
}

ViStatus WRVISA_CALL viSetBuf(ViSession vi, ViUInt16 mask, ViUInt32 size) {
    return wrvisa::api_guard([&] {
        auto session = wrvisa::get_handle<wrvisa::SessionObject>(
            vi, wrvisa::ObjectType::session);
        return session ? session->set_buffer(mask, size) : VI_ERROR_INV_OBJECT;
    });
}

ViStatus WRVISA_CALL viFlush(ViSession vi, ViUInt16 mask) {
    return wrvisa::api_guard([&] {
        auto session = wrvisa::get_handle<wrvisa::SessionObject>(
            vi, wrvisa::ObjectType::session);
        return session ? session->flush(mask) : VI_ERROR_INV_OBJECT;
    });
}

ViStatus WRVISA_CALL wrvisaSetSerialPath(ViSession rmSesn, ViUInt16 intfNum,
                                         ViConstString nativePath) {
    return wrvisa::api_guard([&] {
        if (nativePath == nullptr || nativePath[0] == '\0') {
            return VI_ERROR_INV_PARAMETER;
        }
        auto manager = wrvisa::get_handle<wrvisa::ResourceManager>(
            rmSesn, wrvisa::ObjectType::resource_manager);
        if (!manager) {
            return VI_ERROR_INV_OBJECT;
        }
        return manager->set_serial_path(intfNum, nativePath) ? VI_SUCCESS
                                                              : VI_ERROR_INV_OBJECT;
    });
}

ViStatus WRVISA_CALL wrvisaSetTcpipServicePort(ViSession rmSesn,
                                               ViConstString host,
                                               ViUInt16 protocol,
                                               ViUInt16 port) {
    return wrvisa::api_guard([&] {
        if (host == nullptr || host[0] == '\0' || port == 0) {
            return VI_ERROR_INV_PARAMETER;
        }
        wrvisa::TcpipProtocol selected = wrvisa::TcpipProtocol::unsupported;
        if (protocol == WRVISA_TCPIP_PROTOCOL_VXI11) {
            selected = wrvisa::TcpipProtocol::vxi11;
        } else if (protocol == WRVISA_TCPIP_PROTOCOL_HISLIP) {
            selected = wrvisa::TcpipProtocol::hislip;
        } else {
            return VI_ERROR_INV_PROT;
        }
        auto manager = wrvisa::get_handle<wrvisa::ResourceManager>(
            rmSesn, wrvisa::ObjectType::resource_manager);
        if (!manager) {
            return VI_ERROR_INV_OBJECT;
        }
        return manager->set_tcpip_service_port(host, selected, port)
                   ? VI_SUCCESS
                   : VI_ERROR_INV_PARAMETER;
    });
}

ViStatus WRVISA_CALL wrvisaSetResourceAlias(ViSession rmSesn,
                                            ViConstString alias,
                                            ViConstRsrc resourceName) {
    return wrvisa::api_guard([&] {
        if (alias == nullptr || alias[0] == '\0' || resourceName == nullptr ||
            resourceName[0] == '\0') {
            return VI_ERROR_INV_PARAMETER;
        }
        auto manager = wrvisa::get_handle<wrvisa::ResourceManager>(
            rmSesn, wrvisa::ObjectType::resource_manager);
        if (!manager) {
            return VI_ERROR_INV_OBJECT;
        }
        auto resource = wrvisa::parse_resource(resourceName);
        if (!resource) {
            return VI_ERROR_INV_RSRC_NAME;
        }
        return manager->set_resource_alias(alias, std::move(*resource))
                   ? VI_SUCCESS
                   : VI_ERROR_INV_PARAMETER;
    });
}

}  // extern "C"
