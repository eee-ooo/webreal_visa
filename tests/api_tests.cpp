#include <array>
#include <cstring>
#include <string>

#include "test_support.h"
#include "visa.h"
#include "webreal_visa_ext.h"

int main() {
    ViSession rm = VI_NULL;
    CHECK(viOpenDefaultRM(&rm) == VI_SUCCESS);
    CHECK(rm != VI_NULL);

    ViUInt16 interface_type = 0;
    ViUInt16 interface_number = 99;
    std::array<ViChar, VI_FIND_BUFLEN> resource_class{};
    std::array<ViChar, VI_FIND_BUFLEN> expanded{};
    std::array<ViChar, VI_FIND_BUFLEN> alias{};
    CHECK(viParseRsrcEx(rm, "tcpip::host::5025::socket", &interface_type,
                        &interface_number, resource_class.data(), expanded.data(),
                        alias.data()) == VI_SUCCESS);
    CHECK(interface_type == VI_INTF_TCPIP);
    CHECK(interface_number == 0);
    CHECK(std::string(expanded.data()) == "TCPIP0::host::5025::SOCKET");
    CHECK(alias[0] == '\0');

    ViFindList list = VI_NULL;
    ViUInt32 found_count = 0;
    std::array<ViChar, VI_FIND_BUFLEN> found{};
    CHECK(viFindRsrc(rm, "WRVISA?*", &list, &found_count, found.data()) ==
          VI_ERROR_RSRC_NFOUND);
    CHECK(viFindRsrc(rm, WRVISA_MOCK_FIND_EXPRESSION, &list, &found_count,
                     found.data()) == VI_SUCCESS);
    CHECK(found_count == 1);
    CHECK(std::string(found.data()) == WRVISA_MOCK_RESOURCE);
    CHECK(viFindNext(list, found.data()) == VI_ERROR_RSRC_NFOUND);
    CHECK(viClose(list) == VI_SUCCESS);

    ViSession session = VI_NULL;
    CHECK(viOpen(rm, WRVISA_MOCK_RESOURCE, VI_NO_LOCK, 1000, &session) == VI_SUCCESS);
    CHECK(session != VI_NULL);
    ViSession unsupported_session = VI_NULL;
    CHECK(viOpen(rm, "TCPIP0::localhost::inst0::INSTR", VI_NO_LOCK, 1000,
                 &unsupported_session) == VI_ERROR_NSUP_OPER);

    ViUInt16 actual_type = 0;
    CHECK(viGetAttribute(session, VI_ATTR_INTF_TYPE, &actual_type) == VI_SUCCESS);
    CHECK(actual_type == WRVISA_INTF_MOCK);
    CHECK(viSetAttribute(session, VI_ATTR_TMO_VALUE, 100) == VI_SUCCESS);
    ViUInt32 timeout = 0;
    CHECK(viGetAttribute(session, VI_ATTR_TMO_VALUE, &timeout) == VI_SUCCESS);
    CHECK(timeout == 100);
    CHECK(viSetBuf(session, VI_READ_BUF | VI_WRITE_BUF, 8192) == VI_SUCCESS);

    const std::string query = "*IDN?\n";
    ViUInt32 written = 0;
    CHECK(viWrite(session, reinterpret_cast<ViConstBuf>(query.data()),
                  static_cast<ViUInt32>(query.size()), &written) == VI_SUCCESS);
    CHECK(written == query.size());

    CHECK(viSetAttribute(session, VI_ATTR_TERMCHAR, '\n') == VI_SUCCESS);
    CHECK(viSetAttribute(session, VI_ATTR_TERMCHAR_EN, VI_TRUE) == VI_SUCCESS);
    std::array<ViByte, 128> buffer{};
    ViUInt32 read = 0;
    CHECK(viRead(session, buffer.data(), static_cast<ViUInt32>(buffer.size()), &read) ==
          VI_SUCCESS_TERM_CHAR);
    const std::string identity(reinterpret_cast<const char*>(buffer.data()), read);
    CHECK(identity == "WEBREAL,WRVISA-MOCK,0001,0.2\n");

    ViUInt16 stb = 99;
    CHECK(viReadSTB(session, &stb) == VI_SUCCESS);
    CHECK(stb == 0);
    CHECK(viAssertTrigger(session, VI_TRIG_PROT_DEFAULT) == VI_SUCCESS);
    CHECK(viClear(session) == VI_SUCCESS);
    CHECK(viFlush(session, VI_READ_BUF_DISCARD) == VI_SUCCESS);

    ViSession peer = VI_NULL;
    CHECK(viOpen(rm, WRVISA_MOCK_RESOURCE, VI_NO_LOCK, 1000, &peer) == VI_SUCCESS);
    std::array<ViChar, VI_FIND_BUFLEN> key{};
    CHECK(viLock(session, VI_EXCLUSIVE_LOCK, 0, nullptr, key.data()) == VI_SUCCESS);
    CHECK(viWrite(peer, reinterpret_cast<ViConstBuf>(query.data()),
                  static_cast<ViUInt32>(query.size()), &written) ==
          VI_ERROR_RSRC_LOCKED);
    CHECK(viLock(peer, VI_EXCLUSIVE_LOCK, VI_TMO_IMMEDIATE, nullptr, nullptr) ==
          VI_ERROR_TMO);
    CHECK(viUnlock(session) == VI_SUCCESS);

    CHECK(viLock(session, VI_SHARED_LOCK, 0, nullptr, key.data()) == VI_SUCCESS);
    CHECK(key[0] != '\0');
    CHECK(viLock(peer, VI_SHARED_LOCK, 0, key.data(), nullptr) == VI_SUCCESS);
    CHECK(viUnlock(peer) == VI_SUCCESS);
    CHECK(viUnlock(session) == VI_SUCCESS);

    std::array<ViChar, VI_FIND_BUFLEN> description{};
    CHECK(viStatusDesc(rm, VI_ERROR_TMO, description.data()) == VI_SUCCESS);
    CHECK(std::string(description.data()).find("timed out") != std::string::npos);

    CHECK(viClose(peer) == VI_SUCCESS);
    CHECK(viClose(session) == VI_SUCCESS);
    CHECK(viClose(session) == VI_ERROR_INV_OBJECT);
    CHECK(viClose(rm) == VI_SUCCESS);
    return 0;
}
