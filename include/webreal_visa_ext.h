#ifndef WEBREAL_VISA_EXT_H
#define WEBREAL_VISA_EXT_H

#include "visa.h"

#define WRVISA_VERSION_MAJOR 0
#define WRVISA_VERSION_MINOR 4
#define WRVISA_VERSION_PATCH 0
#define WRVISA_VERSION_STRING "0.4.0"

/* PROJECT_EXTENSION: isolated test transport, never a physical VISA resource. */
#define WRVISA_MOCK_RESOURCE "WRVISA0::MOCK::INSTR"
#define WRVISA_MOCK_FIND_EXPRESSION "WRVISA0?*"
#define WRVISA_INTF_MOCK UINT16_C(0x8000)

#define WRVISA_TCPIP_PROTOCOL_VXI11 1
#define WRVISA_TCPIP_PROTOCOL_HISLIP 2

#ifdef __cplusplus
extern "C" {
#endif

/* PROJECT_EXTENSION: map a standard ASRL interface number to a native path. */
WRVISA_API ViStatus WRVISA_CALL wrvisaSetSerialPath(ViSession rmSesn,
                                                    ViUInt16 intfNum,
                                                    ViConstString nativePath);

/* PROJECT_EXTENSION: override a TCPIP INSTR service port for one RM and host. */
WRVISA_API ViStatus WRVISA_CALL wrvisaSetTcpipServicePort(
    ViSession rmSesn, ViConstString host, ViUInt16 protocol, ViUInt16 port);

/* PROJECT_EXTENSION: define one process-local resource alias for this RM. */
WRVISA_API ViStatus WRVISA_CALL wrvisaSetResourceAlias(
    ViSession rmSesn, ViConstString alias, ViConstRsrc resourceName);

#ifdef __cplusplus
}
#endif

#endif
