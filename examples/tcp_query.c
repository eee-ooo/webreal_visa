#include <stdio.h>
#include <string.h>

#include "visa.h"

int main(int argc, char** argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: tcp_query TCPIP0::host::port::SOCKET\n");
        return 2;
    }

    ViSession rm = VI_NULL;
    ViSession instrument = VI_NULL;
    if (viOpenDefaultRM(&rm) < VI_SUCCESS ||
        viOpen(rm, argv[1], VI_NO_LOCK, 3000, &instrument) < VI_SUCCESS) {
        fprintf(stderr, "failed to open %s\n", argv[1]);
        if (rm != VI_NULL) {
            viClose(rm);
        }
        return 1;
    }

    viSetAttribute(instrument, VI_ATTR_TMO_VALUE, 3000);
    viSetAttribute(instrument, VI_ATTR_TERMCHAR, '\n');
    viSetAttribute(instrument, VI_ATTR_TERMCHAR_EN, VI_TRUE);

    static const char query[] = "*IDN?\n";
    ViUInt32 transferred = 0;
    ViByte response[1024];
    ViStatus status = viWrite(instrument, (ViConstBuf)query,
                              (ViUInt32)(sizeof(query) - 1u), &transferred);
    if (status >= VI_SUCCESS) {
        status = viRead(instrument, response, (ViUInt32)(sizeof(response) - 1u),
                        &transferred);
    }
    if (status >= VI_SUCCESS) {
        response[transferred] = 0;
        fputs((const char*)response, stdout);
    } else {
        fprintf(stderr, "I/O failed with VISA status 0x%08X\n",
                (unsigned int)status);
    }

    viClose(instrument);
    viClose(rm);
    return status >= VI_SUCCESS ? 0 : 1;
}
