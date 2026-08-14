#include <stdio.h>
#include <string.h>

#include "visa.h"
#include "webreal_visa_ext.h"

int main(void) {
    ViSession manager = VI_NULL;
    ViSession session = VI_NULL;
    const char query[] = "*IDN?\n";
    ViByte response[128] = {0};
    ViUInt32 count = 0;

    if (viOpenDefaultRM(&manager) != VI_SUCCESS ||
        viOpen(manager, WRVISA_MOCK_RESOURCE, VI_NO_LOCK, 1000, &session) != VI_SUCCESS ||
        viWrite(session, (ViConstBuf)query, (ViUInt32)(sizeof(query) - 1u), &count) != VI_SUCCESS ||
        viRead(session, response, (ViUInt32)(sizeof(response) - 1u), &count) < VI_SUCCESS) {
        return 1;
    }
    response[count] = 0;
    printf("%s", (const char*)response);
    if (strcmp((const char*)response, "WEBREAL,WRVISA-MOCK,0001,0.6\n") != 0) {
        return 2;
    }
    return viClose(session) == VI_SUCCESS && viClose(manager) == VI_SUCCESS ? 0 : 3;
}
