#ifndef WEBREAL_VISA_VISA_H
#define WEBREAL_VISA_VISA_H

#include <stddef.h>
#include <stdint.h>

#if defined(_WIN32)
#  if defined(WRVISA_BUILDING_LIBRARY)
#    define WRVISA_API __declspec(dllexport)
#  elif defined(WRVISA_SHARED_LIBRARY)
#    define WRVISA_API __declspec(dllimport)
#  else
#    define WRVISA_API
#  endif
#  if defined(_M_IX86) || defined(__i386__)
#    define WRVISA_CALL __stdcall
#  else
#    define WRVISA_CALL
#  endif
#else
#  if defined(WRVISA_BUILDING_LIBRARY)
#    define WRVISA_API __attribute__((visibility("default")))
#  else
#    define WRVISA_API
#  endif
#  define WRVISA_CALL
#endif

#define VI_SPEC_VERSION UINT32_C(0x00700200)
#define VI_FIND_BUFLEN 256

typedef uint8_t ViUInt8;
typedef int8_t ViInt8;
typedef uint16_t ViUInt16;
typedef int16_t ViInt16;
typedef uint32_t ViUInt32;
typedef int32_t ViInt32;
typedef uint64_t ViUInt64;
typedef int64_t ViInt64;
typedef float ViReal32;
typedef double ViReal64;
typedef char ViChar;
typedef unsigned char ViByte;
typedef void* ViAddr;
typedef ViUInt8* ViPUInt8;
typedef ViUInt16* ViPUInt16;
typedef ViUInt32* ViPUInt32;
typedef ViInt32* ViPInt32;
typedef ViByte* ViBuf;
typedef const ViByte* ViConstBuf;
typedef ViByte* ViPBuf;
typedef ViChar* ViString;
typedef const ViChar* ViConstString;
typedef ViChar* ViPString;
typedef ViString ViRsrc;
typedef ViConstString ViConstRsrc;
typedef ViUInt16 ViBoolean;
typedef ViInt32 ViStatus;
typedef ViUInt32 ViVersion;
typedef ViUInt32 ViObject;
typedef ViObject ViSession;
typedef ViSession* ViPSession;
typedef ViObject ViFindList;
typedef ViFindList* ViPFindList;
typedef ViUInt32 ViAttr;
typedef uintptr_t ViAttrState;
typedef void* ViPAttrState;
typedef ViString ViKeyId;
typedef ViConstString ViConstKeyId;
typedef ViPString ViPKeyId;
typedef ViUInt32 ViJobId;
typedef ViUInt32 ViAccessMode;

#define VI_NULL 0
#define VI_TRUE 1
#define VI_FALSE 0

#define VI_INTF_GPIB 1
#define VI_INTF_VXI 2
#define VI_INTF_GPIB_VXI 3
#define VI_INTF_ASRL 4
#define VI_INTF_PXI 5
#define VI_INTF_TCPIP 6
#define VI_INTF_USB 7

#define VI_ATTR_RSRC_CLASS UINT32_C(0xBFFF0001)
#define VI_ATTR_RSRC_NAME UINT32_C(0xBFFF0002)
#define VI_ATTR_RSRC_IMPL_VERSION UINT32_C(0x3FFF0003)
#define VI_ATTR_RSRC_LOCK_STATE UINT32_C(0x3FFF0004)
#define VI_ATTR_USER_DATA UINT32_C(0x3FFF0007)
#define VI_ATTR_SEND_END_EN UINT32_C(0x3FFF0016)
#define VI_ATTR_TERMCHAR UINT32_C(0x3FFF0018)
#define VI_ATTR_TMO_VALUE UINT32_C(0x3FFF001A)
#define VI_ATTR_RD_BUF_SIZE UINT32_C(0x3FFF002B)
#define VI_ATTR_WR_BUF_SIZE UINT32_C(0x3FFF002E)
#define VI_ATTR_TERMCHAR_EN UINT32_C(0x3FFF0038)
#define VI_ATTR_ASRL_BAUD UINT32_C(0x3FFF0021)
#define VI_ATTR_ASRL_DATA_BITS UINT32_C(0x3FFF0022)
#define VI_ATTR_ASRL_PARITY UINT32_C(0x3FFF0023)
#define VI_ATTR_ASRL_STOP_BITS UINT32_C(0x3FFF0024)
#define VI_ATTR_ASRL_FLOW_CNTRL UINT32_C(0x3FFF0025)
#define VI_ATTR_RM_SESSION UINT32_C(0x3FFF00C4)
#define VI_ATTR_RSRC_SPEC_VERSION UINT32_C(0x3FFF0170)
#define VI_ATTR_INTF_TYPE UINT32_C(0x3FFF0171)
#define VI_ATTR_INTF_NUM UINT32_C(0x3FFF0176)
#define VI_ATTR_TCPIP_ADDR UINT32_C(0xBFFF0195)
#define VI_ATTR_TCPIP_HOSTNAME UINT32_C(0xBFFF0196)
#define VI_ATTR_TCPIP_PORT UINT32_C(0x3FFF0197)
#define VI_ATTR_TCPIP_NODELAY UINT32_C(0x3FFF019A)
#define VI_ATTR_TCPIP_KEEPALIVE UINT32_C(0x3FFF019B)

#define VI_SUCCESS INT32_C(0)
#define VI_SUCCESS_TERM_CHAR INT32_C(0x3FFF0005)
#define VI_SUCCESS_MAX_CNT INT32_C(0x3FFF0006)
#define VI_SUCCESS_NESTED_SHARED INT32_C(0x3FFF0099)
#define VI_SUCCESS_NESTED_EXCLUSIVE INT32_C(0x3FFF009A)

#define VI_ERROR_SYSTEM_ERROR ((ViStatus)UINT32_C(0xBFFF0000))
#define VI_ERROR_INV_OBJECT ((ViStatus)UINT32_C(0xBFFF000E))
#define VI_ERROR_RSRC_LOCKED ((ViStatus)UINT32_C(0xBFFF000F))
#define VI_ERROR_INV_EXPR ((ViStatus)UINT32_C(0xBFFF0010))
#define VI_ERROR_RSRC_NFOUND ((ViStatus)UINT32_C(0xBFFF0011))
#define VI_ERROR_INV_RSRC_NAME ((ViStatus)UINT32_C(0xBFFF0012))
#define VI_ERROR_INV_ACC_MODE ((ViStatus)UINT32_C(0xBFFF0013))
#define VI_ERROR_TMO ((ViStatus)UINT32_C(0xBFFF0015))
#define VI_ERROR_INV_DEGREE ((ViStatus)UINT32_C(0xBFFF001B))
#define VI_ERROR_INV_JOB_ID ((ViStatus)UINT32_C(0xBFFF001C))
#define VI_ERROR_NSUP_ATTR ((ViStatus)UINT32_C(0xBFFF001D))
#define VI_ERROR_NSUP_ATTR_STATE ((ViStatus)UINT32_C(0xBFFF001E))
#define VI_ERROR_ATTR_READONLY ((ViStatus)UINT32_C(0xBFFF001F))
#define VI_ERROR_INV_LOCK_TYPE ((ViStatus)UINT32_C(0xBFFF0020))
#define VI_ERROR_INV_ACCESS_KEY ((ViStatus)UINT32_C(0xBFFF0021))
#define VI_ERROR_ABORT ((ViStatus)UINT32_C(0xBFFF0030))
#define VI_ERROR_ALLOC ((ViStatus)UINT32_C(0xBFFF003C))
#define VI_ERROR_INV_MASK ((ViStatus)UINT32_C(0xBFFF003D))
#define VI_ERROR_IO ((ViStatus)UINT32_C(0xBFFF003E))
#define VI_ERROR_NSUP_OPER ((ViStatus)UINT32_C(0xBFFF0067))
#define VI_ERROR_RSRC_BUSY ((ViStatus)UINT32_C(0xBFFF0072))
#define VI_ERROR_INV_PARAMETER ((ViStatus)UINT32_C(0xBFFF0078))
#define VI_ERROR_INV_PROT ((ViStatus)UINT32_C(0xBFFF0079))
#define VI_ERROR_INV_SIZE ((ViStatus)UINT32_C(0xBFFF007B))
#define VI_ERROR_SESN_NLOCKED ((ViStatus)UINT32_C(0xBFFF009C))
#define VI_ERROR_INTF_NUM_NCONFIG ((ViStatus)UINT32_C(0xBFFF00A5))
#define VI_ERROR_CONN_LOST ((ViStatus)UINT32_C(0xBFFF00A6))
#define VI_ERROR_NPERMISSION ((ViStatus)UINT32_C(0xBFFF00A8))

#define VI_READ_BUF 1
#define VI_WRITE_BUF 2
#define VI_READ_BUF_DISCARD 4
#define VI_WRITE_BUF_DISCARD 8
#define VI_IO_IN_BUF 16
#define VI_IO_OUT_BUF 32
#define VI_IO_IN_BUF_DISCARD 64
#define VI_IO_OUT_BUF_DISCARD 128

#define VI_TMO_IMMEDIATE UINT32_C(0)
#define VI_TMO_INFINITE UINT32_C(0xFFFFFFFF)
#define VI_NO_LOCK 0
#define VI_EXCLUSIVE_LOCK 1
#define VI_SHARED_LOCK 2
#define VI_LOAD_CONFIG 4

#define VI_TRIG_PROT_DEFAULT 0

#define VI_ASRL_PAR_NONE 0
#define VI_ASRL_PAR_ODD 1
#define VI_ASRL_PAR_EVEN 2
#define VI_ASRL_PAR_MARK 3
#define VI_ASRL_PAR_SPACE 4
#define VI_ASRL_STOP_ONE 10
#define VI_ASRL_STOP_ONE5 15
#define VI_ASRL_STOP_TWO 20
#define VI_ASRL_FLOW_NONE 0
#define VI_ASRL_FLOW_XON_XOFF 1
#define VI_ASRL_FLOW_RTS_CTS 2
#define VI_ASRL_FLOW_DTR_DSR 4

#ifdef __cplusplus
extern "C" {
#endif

WRVISA_API ViStatus WRVISA_CALL viOpenDefaultRM(ViPSession vi);
WRVISA_API ViStatus WRVISA_CALL viGetDefaultRM(ViPSession vi);
WRVISA_API ViStatus WRVISA_CALL viFindRsrc(ViSession sesn, ViConstString expr,
                                           ViPFindList vi, ViPUInt32 retCnt,
                                           ViChar desc[]);
WRVISA_API ViStatus WRVISA_CALL viFindNext(ViFindList vi, ViChar desc[]);
WRVISA_API ViStatus WRVISA_CALL viParseRsrc(ViSession rmSesn, ViConstRsrc rsrcName,
                                            ViPUInt16 intfType, ViPUInt16 intfNum);
WRVISA_API ViStatus WRVISA_CALL viParseRsrcEx(ViSession rmSesn, ViConstRsrc rsrcName,
                                              ViPUInt16 intfType, ViPUInt16 intfNum,
                                              ViChar rsrcClass[],
                                              ViChar expandedUnaliasedName[],
                                              ViChar aliasIfExists[]);
WRVISA_API ViStatus WRVISA_CALL viOpen(ViSession sesn, ViConstRsrc name,
                                       ViAccessMode mode, ViUInt32 timeout,
                                       ViPSession vi);
WRVISA_API ViStatus WRVISA_CALL viClose(ViObject vi);
WRVISA_API ViStatus WRVISA_CALL viSetAttribute(ViObject vi, ViAttr attrName,
                                               ViAttrState attrValue);
WRVISA_API ViStatus WRVISA_CALL viGetAttribute(ViObject vi, ViAttr attrName,
                                               void* attrValue);
WRVISA_API ViStatus WRVISA_CALL viStatusDesc(ViObject vi, ViStatus status,
                                             ViChar desc[]);
WRVISA_API ViStatus WRVISA_CALL viTerminate(ViObject vi, ViUInt16 degree,
                                            ViJobId jobId);
WRVISA_API ViStatus WRVISA_CALL viLock(ViSession vi, ViAccessMode lockType,
                                       ViUInt32 timeout, ViConstKeyId requestedKey,
                                       ViChar accessKey[]);
WRVISA_API ViStatus WRVISA_CALL viUnlock(ViSession vi);
WRVISA_API ViStatus WRVISA_CALL viRead(ViSession vi, ViPBuf buf, ViUInt32 cnt,
                                       ViPUInt32 retCnt);
WRVISA_API ViStatus WRVISA_CALL viWrite(ViSession vi, ViConstBuf buf, ViUInt32 cnt,
                                        ViPUInt32 retCnt);
WRVISA_API ViStatus WRVISA_CALL viAssertTrigger(ViSession vi, ViUInt16 protocol);
WRVISA_API ViStatus WRVISA_CALL viReadSTB(ViSession vi, ViPUInt16 status);
WRVISA_API ViStatus WRVISA_CALL viClear(ViSession vi);
WRVISA_API ViStatus WRVISA_CALL viSetBuf(ViSession vi, ViUInt16 mask, ViUInt32 size);
WRVISA_API ViStatus WRVISA_CALL viFlush(ViSession vi, ViUInt16 mask);

#ifdef __cplusplus
}
#endif

#endif
