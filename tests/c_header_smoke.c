#include <stdint.h>

#include "visa.h"
#include "webreal_visa_ext.h"
#include "webreal_visa_plugin.h"

_Static_assert(sizeof(ViUInt16) == 2, "ViUInt16 ABI");
_Static_assert(sizeof(ViUInt32) == 4, "ViUInt32 ABI");
_Static_assert(sizeof(ViStatus) == 4, "ViStatus ABI");
_Static_assert(sizeof(ViSession) == 4, "ViSession ABI");
_Static_assert(sizeof(ViAttrState) == sizeof(uintptr_t), "ViAttrState ABI");
_Static_assert(VI_ATTR_ASRL_BAUD == UINT32_C(0x3FFF0021), "ASRL attribute ABI");
_Static_assert(VI_ATTR_TCPIP_PORT == UINT32_C(0x3FFF0197), "TCPIP attribute ABI");
_Static_assert(WRVISA_VERSION_MINOR == 2, "extension version");

int main(void) {
    ViSession session = VI_NULL;
    return viGetDefaultRM(&session) == VI_SUCCESS && viClose(session) == VI_SUCCESS ? 0 : 1;
}
