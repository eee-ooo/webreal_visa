#ifndef WEBREAL_VISA_EXT_H
#define WEBREAL_VISA_EXT_H

#include "visa.h"

#define WRVISA_VERSION_MAJOR 0
#define WRVISA_VERSION_MINOR 6
#define WRVISA_VERSION_PATCH 0
#define WRVISA_VERSION_STRING "0.6.0"

/* PROJECT_EXTENSION: isolated test transport, never a physical VISA resource. */
#define WRVISA_MOCK_RESOURCE "WRVISA0::MOCK::INSTR"
#define WRVISA_MOCK_FIND_EXPRESSION "WRVISA0?*"
#define WRVISA_INTF_MOCK UINT16_C(0x8000)

#define WRVISA_TCPIP_PROTOCOL_VXI11 1
#define WRVISA_TCPIP_PROTOCOL_HISLIP 2

/* DE_FACTO_EXTENSION: versioned USB RAW endpoint/control contract. */
#define WRVISA_USB_RAW_ABI_MAJOR 1
#define WRVISA_USB_RAW_ABI_MINOR 0

#define WRVISA_USB_TRANSFER_NONE 0
#define WRVISA_USB_TRANSFER_BULK 1
#define WRVISA_USB_TRANSFER_INTERRUPT 2

/* PROJECT_EXTENSION: explicit Prologix serial/TCP controller configuration. */
#define WRVISA_PROLOGIX_ABI_MAJOR 1
#define WRVISA_PROLOGIX_ABI_MINOR 0

#define WRVISA_PROLOGIX_CONNECTION_SERIAL 1
#define WRVISA_PROLOGIX_CONNECTION_TCP 2

#define WRVISA_PROLOGIX_MAX_RESPONSE_SIZE UINT32_C(67108864)

typedef struct wrvisa_usb_raw_config_v1 {
    ViUInt32 struct_size;
    ViUInt16 abi_major;
    ViUInt16 abi_minor;
    ViUInt8 alternate_setting;
    ViUInt8 read_transfer_type;
    ViUInt8 read_endpoint;
    ViUInt8 write_transfer_type;
    ViUInt8 write_endpoint;
    ViUInt8 reserved8[3];
    ViUInt32 flags;
    ViUInt32 reserved[3];
} wrvisa_usb_raw_config_v1;

typedef struct wrvisa_usb_control_request_v1 {
    ViUInt32 struct_size;
    ViUInt16 abi_major;
    ViUInt16 abi_minor;
    ViUInt8 request_type;
    ViUInt8 request;
    ViUInt16 value;
    ViUInt16 index;
    ViUInt16 reserved16;
    ViUInt32 flags;
    ViUInt32 reserved[3];
} wrvisa_usb_control_request_v1;

typedef struct wrvisa_prologix_controller_config_v1 {
    ViUInt32 struct_size;
    ViUInt16 abi_major;
    ViUInt16 abi_minor;
    ViUInt16 connection_type;
    ViUInt16 tcp_port;
    ViUInt8 eot_char;
    ViUInt8 reserved8[3];
    ViUInt32 read_timeout_ms;
    ViUInt32 maximum_response_size;
    ViUInt32 flags;
    ViUInt32 reserved[3];
} wrvisa_prologix_controller_config_v1;

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

/* DE_FACTO_EXTENSION: configure one USB RAW resource before viOpen(). */
WRVISA_API ViStatus WRVISA_CALL wrvisaSetUsbRawConfig(
    ViSession rmSesn, ViConstRsrc resourceName,
    const wrvisa_usb_raw_config_v1* config);

/* DE_FACTO_EXTENSION: execute an endpoint-zero transfer on a USB RAW session. */
WRVISA_API ViStatus WRVISA_CALL wrvisaUsbControlTransfer(
    ViSession vi, const wrvisa_usb_control_request_v1* request,
    ViBuf data, ViUInt32 count, ViPUInt32 retCnt);

/* PROJECT_EXTENSION: map one GPIB board to an explicit Prologix endpoint. */
WRVISA_API ViStatus WRVISA_CALL wrvisaSetPrologixController(
    ViSession rmSesn, ViUInt16 board, ViConstString endpoint,
    const wrvisa_prologix_controller_config_v1* config);

#ifdef __cplusplus
}
#endif

#endif
