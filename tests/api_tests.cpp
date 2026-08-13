#include <array>
#include <cstdint>
#include <cstring>
#include <string>

#include "test_support.h"
#include "visa.h"
#include "webreal_visa_ext.h"

int main() {
    CHECK(viOpenDefaultRM(nullptr) == VI_ERROR_INV_PARAMETER);

    ViSession rm = VI_NULL;
    CHECK(viOpenDefaultRM(&rm) == VI_SUCCESS);
    CHECK(rm != VI_NULL);

    ViSession second_rm = VI_NULL;
    CHECK(viGetDefaultRM(&second_rm) == VI_SUCCESS);
    CHECK(second_rm != VI_NULL);
    CHECK(second_rm != rm);
    CHECK(viClose(second_rm) == VI_SUCCESS);

    std::array<ViChar, VI_FIND_BUFLEN> rm_class{};
    ViVersion version = 0;
    CHECK(viGetAttribute(rm, VI_ATTR_RSRC_CLASS, rm_class.data()) == VI_SUCCESS);
    CHECK(std::string(rm_class.data()) == "RM");
    CHECK(viGetAttribute(rm, VI_ATTR_RSRC_IMPL_VERSION, &version) == VI_SUCCESS);
    CHECK(version == UINT32_C(0x00000500));
    CHECK(viGetAttribute(rm, VI_ATTR_RSRC_SPEC_VERSION, &version) == VI_SUCCESS);
    CHECK(version == VI_SPEC_VERSION);
    CHECK(viGetAttribute(rm, VI_ATTR_USER_DATA, &version) == VI_ERROR_NSUP_ATTR);
    CHECK(viGetAttribute(rm, VI_ATTR_RSRC_CLASS, nullptr) ==
          VI_ERROR_INV_PARAMETER);

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
    CHECK(viParseRsrcEx(rm, WRVISA_MOCK_RESOURCE, &interface_type,
                        &interface_number, nullptr, nullptr, nullptr) == VI_SUCCESS);

    CHECK(wrvisaSetResourceAlias(VI_NULL, "scope.mock", WRVISA_MOCK_RESOURCE) ==
          VI_ERROR_INV_OBJECT);
    CHECK(wrvisaSetResourceAlias(rm, nullptr, WRVISA_MOCK_RESOURCE) ==
          VI_ERROR_INV_PARAMETER);
    CHECK(wrvisaSetResourceAlias(rm, "bad:alias", WRVISA_MOCK_RESOURCE) ==
          VI_ERROR_INV_PARAMETER);
    CHECK(wrvisaSetResourceAlias(rm, "ASRL2", WRVISA_MOCK_RESOURCE) ==
          VI_ERROR_INV_PARAMETER);
    CHECK(wrvisaSetResourceAlias(rm, "scope.mock", "not-a-resource") ==
          VI_ERROR_INV_RSRC_NAME);
    CHECK(wrvisaSetResourceAlias(rm, "scope.mock", WRVISA_MOCK_RESOURCE) ==
          VI_SUCCESS);
    CHECK(viParseRsrc(rm, "SCOPE.MOCK", &interface_type, &interface_number) ==
          VI_SUCCESS);
    CHECK(interface_type == WRVISA_INTF_MOCK);
    CHECK(viParseRsrcEx(rm, "scope.mock", &interface_type, &interface_number,
                        resource_class.data(), expanded.data(), alias.data()) ==
          VI_SUCCESS);
    CHECK(std::string(expanded.data()) == WRVISA_MOCK_RESOURCE);
    CHECK(std::string(alias.data()) == "scope.mock");
    alias.fill(0);
    CHECK(viParseRsrcEx(rm, WRVISA_MOCK_RESOURCE, &interface_type,
                        &interface_number, resource_class.data(), expanded.data(),
                        alias.data()) == VI_SUCCESS);
    CHECK(std::string(alias.data()) == "scope.mock");
    ViSession isolated_rm = VI_NULL;
    CHECK(viOpenDefaultRM(&isolated_rm) == VI_SUCCESS);
    CHECK(viParseRsrc(isolated_rm, "scope.mock", &interface_type,
                      &interface_number) == VI_ERROR_INV_RSRC_NAME);
    CHECK(viClose(isolated_rm) == VI_SUCCESS);
    CHECK(wrvisaSetResourceAlias(rm, "scope.other", WRVISA_MOCK_RESOURCE) ==
          VI_SUCCESS);
    CHECK(viParseRsrc(rm, "scope.mock", &interface_type, &interface_number) ==
          VI_ERROR_INV_RSRC_NAME);
    CHECK(viParseRsrc(rm, "SCOPE.OTHER", &interface_type, &interface_number) ==
          VI_SUCCESS);
    CHECK(wrvisaSetResourceAlias(rm, "scope.mock", WRVISA_MOCK_RESOURCE) ==
          VI_SUCCESS);
    CHECK(viParseRsrc(rm, "scope.other", &interface_type, &interface_number) ==
          VI_ERROR_INV_RSRC_NAME);

    ViFindList list = VI_NULL;
    ViUInt32 found_count = 0;
    std::array<ViChar, VI_FIND_BUFLEN> found{};
    CHECK(viFindRsrc(rm, nullptr, &list, &found_count, found.data()) ==
          VI_ERROR_INV_PARAMETER);
    CHECK(viFindRsrc(rm, "[", &list, &found_count, found.data()) ==
          VI_ERROR_INV_EXPR);
    CHECK(viFindRsrc(rm, "WRVISA0?*{VI_ATTR_INTF_TYPE == 0x8000}", nullptr,
                     nullptr, found.data()) == VI_SUCCESS);
    CHECK(std::string(found.data()) == WRVISA_MOCK_RESOURCE);
    CHECK(viFindRsrc(rm,
                     "WRVISA0?*{VI_ATTR_RSRC_CLASS == \"INSTR\" && "
                     "VI_ATTR_INTF_NUM == 0}",
                     nullptr, &found_count, found.data()) == VI_SUCCESS);
    CHECK(found_count == 1);
    CHECK(viFindRsrc(rm, "WRVISA0?*{VI_ATTR_INTF_NUM != 0}", nullptr,
                     nullptr, found.data()) == VI_ERROR_RSRC_NFOUND);
    CHECK(viFindRsrc(rm, "WRVISA0?*{VI_ATTR_TCPIP_PORT == 1}", nullptr,
                     nullptr, found.data()) == VI_ERROR_INV_EXPR);
    CHECK(viFindRsrc(VI_NULL, WRVISA_MOCK_FIND_EXPRESSION, &list, &found_count,
                     found.data()) == VI_ERROR_INV_OBJECT);
    CHECK(viFindNext(VI_NULL, found.data()) == VI_ERROR_INV_OBJECT);
    CHECK(viFindNext(VI_NULL, nullptr) == VI_ERROR_INV_PARAMETER);
    CHECK(viFindRsrc(rm, "WRVISA?*", &list, &found_count, found.data()) ==
          VI_ERROR_RSRC_NFOUND);
    CHECK(viFindRsrc(rm, WRVISA_MOCK_FIND_EXPRESSION, &list, &found_count,
                     found.data()) == VI_SUCCESS);
    CHECK(found_count == 1);
    CHECK(std::string(found.data()) == WRVISA_MOCK_RESOURCE);
    CHECK(viFindNext(list, found.data()) == VI_ERROR_RSRC_NFOUND);
    CHECK(viClose(list) == VI_SUCCESS);

    CHECK(viParseRsrc(rm, nullptr, &interface_type, &interface_number) ==
          VI_ERROR_INV_PARAMETER);
    CHECK(viParseRsrc(rm, "not-a-resource", &interface_type, &interface_number) ==
          VI_ERROR_INV_RSRC_NAME);
    CHECK(viParseRsrc(VI_NULL, WRVISA_MOCK_RESOURCE, &interface_type,
                      &interface_number) == VI_ERROR_INV_OBJECT);
    CHECK(viParseRsrcEx(rm, WRVISA_MOCK_RESOURCE, nullptr,
                        &interface_number, nullptr, expanded.data(), alias.data()) ==
          VI_ERROR_INV_PARAMETER);

    ViSession session = VI_NULL;
    CHECK(viOpen(rm, nullptr, VI_NO_LOCK, 1000, &session) ==
          VI_ERROR_INV_PARAMETER);
    CHECK(viOpen(rm, WRVISA_MOCK_RESOURCE,
                 VI_EXCLUSIVE_LOCK | VI_SHARED_LOCK, 1000, &session) ==
          VI_ERROR_INV_ACC_MODE);
    CHECK(viOpen(VI_NULL, WRVISA_MOCK_RESOURCE, VI_NO_LOCK, 1000, &session) ==
          VI_ERROR_INV_OBJECT);
    CHECK(viOpen(rm, "not-a-resource", VI_NO_LOCK, 1000, &session) ==
          VI_ERROR_INV_RSRC_NAME);
    CHECK(viOpen(rm, "GPIB0::1::INSTR", VI_NO_LOCK, 1000, &session) ==
          VI_ERROR_NSUP_OPER);
    CHECK(viOpen(rm, WRVISA_MOCK_RESOURCE, VI_NO_LOCK, 1000, &session) == VI_SUCCESS);
    CHECK(session != VI_NULL);
    ViSession alias_session = VI_NULL;
    CHECK(viOpen(rm, "ScOpE.mOcK", VI_NO_LOCK, 1000, &alias_session) == VI_SUCCESS);
    CHECK(viClose(alias_session) == VI_SUCCESS);
    ViSession unsupported_session = VI_NULL;
    CHECK(viOpen(rm, "TCPIP0::localhost::vendor0::INSTR", VI_NO_LOCK, 1000,
                 &unsupported_session) == VI_ERROR_NSUP_OPER);

    ViUInt16 actual_type = 0;
    CHECK(viGetAttribute(session, VI_ATTR_INTF_TYPE, &actual_type) == VI_SUCCESS);
    CHECK(actual_type == WRVISA_INTF_MOCK);
    CHECK(viSetAttribute(session, VI_ATTR_TMO_VALUE, 100) == VI_SUCCESS);
    ViUInt32 timeout = 0;
    CHECK(viGetAttribute(session, VI_ATTR_TMO_VALUE, &timeout) == VI_SUCCESS);
    CHECK(timeout == 100);
    CHECK(viSetAttribute(session, VI_ATTR_TERMCHAR,
                         static_cast<ViAttrState>(UINT16_MAX)) ==
          VI_ERROR_NSUP_ATTR_STATE);
    CHECK(viSetAttribute(session, VI_ATTR_TERMCHAR_EN, 2) ==
          VI_ERROR_NSUP_ATTR_STATE);
    CHECK(viSetAttribute(session, VI_ATTR_SEND_END_EN, 2) ==
          VI_ERROR_NSUP_ATTR_STATE);
    CHECK(viSetAttribute(session, VI_ATTR_RSRC_NAME, 0) ==
          VI_ERROR_ATTR_READONLY);
    CHECK(viSetAttribute(session, UINT32_C(0xDEADBEEF), 0) ==
          VI_ERROR_NSUP_ATTR);
    CHECK(viGetAttribute(session, UINT32_C(0xDEADBEEF), &timeout) ==
          VI_ERROR_NSUP_ATTR);
    CHECK(viGetAttribute(session, VI_ATTR_TMO_VALUE, nullptr) ==
          VI_ERROR_INV_PARAMETER);
    CHECK(viSetBuf(session, VI_READ_BUF, 0) == VI_ERROR_INV_SIZE);
    CHECK(viSetBuf(session, 0, 8192) == VI_ERROR_INV_MASK);
    CHECK(viSetBuf(session, UINT16_C(0x8000), 8192) == VI_ERROR_INV_MASK);
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
    CHECK(viRead(session, nullptr, 1, &read) == VI_ERROR_INV_PARAMETER);
    CHECK(viRead(session, buffer.data(), 0, &read) == VI_ERROR_INV_PARAMETER);
    CHECK(viWrite(session, nullptr, 1, &written) == VI_ERROR_INV_PARAMETER);
    CHECK(viWrite(session, reinterpret_cast<ViConstBuf>(query.data()), 0,
                  &written) == VI_ERROR_INV_PARAMETER);
    CHECK(viRead(session, buffer.data(), static_cast<ViUInt32>(buffer.size()), &read) ==
          VI_SUCCESS_TERM_CHAR);
    const std::string identity(reinterpret_cast<const char*>(buffer.data()), read);
    CHECK(identity == "WEBREAL,WRVISA-MOCK,0001,0.5\n");

    ViUInt16 stb = 99;
    CHECK(viReadSTB(session, &stb) == VI_SUCCESS);
    CHECK(stb == 0);
    CHECK(viReadSTB(session, nullptr) == VI_ERROR_INV_PARAMETER);
    CHECK(viAssertTrigger(session, 1) == VI_ERROR_INV_PROT);
    CHECK(viAssertTrigger(session, VI_TRIG_PROT_DEFAULT) == VI_SUCCESS);
    CHECK(viClear(session) == VI_SUCCESS);
    CHECK(viFlush(session, 0) == VI_ERROR_INV_MASK);
    CHECK(viFlush(session, VI_READ_BUF_DISCARD) == VI_SUCCESS);
    CHECK(viTerminate(session, 1, VI_NULL) == VI_ERROR_INV_DEGREE);
    CHECK(viTerminate(session, VI_NULL, 1) == VI_ERROR_INV_JOB_ID);
    CHECK(viTerminate(session, VI_NULL, VI_NULL) == VI_SUCCESS);

    ViSession peer = VI_NULL;
    CHECK(viOpen(rm, WRVISA_MOCK_RESOURCE, VI_NO_LOCK, 1000, &peer) == VI_SUCCESS);
    std::array<ViChar, VI_FIND_BUFLEN> key{};
    CHECK(viUnlock(session) == VI_ERROR_SESN_NLOCKED);
    CHECK(viLock(session, VI_NO_LOCK, 0, nullptr, nullptr) ==
          VI_ERROR_INV_LOCK_TYPE);
    CHECK(viLock(session, VI_EXCLUSIVE_LOCK, 0, "unexpected", nullptr) ==
          VI_ERROR_INV_ACCESS_KEY);
    CHECK(viLock(session, VI_EXCLUSIVE_LOCK, 0, nullptr, key.data()) == VI_SUCCESS);
    CHECK(viLock(session, VI_EXCLUSIVE_LOCK, 0, nullptr, nullptr) ==
          VI_SUCCESS_NESTED_EXCLUSIVE);
    CHECK(viWrite(peer, reinterpret_cast<ViConstBuf>(query.data()),
                  static_cast<ViUInt32>(query.size()), &written) ==
          VI_ERROR_RSRC_LOCKED);
    CHECK(viLock(peer, VI_EXCLUSIVE_LOCK, VI_TMO_IMMEDIATE, nullptr, nullptr) ==
          VI_ERROR_TMO);
    CHECK(viUnlock(session) == VI_SUCCESS);
    CHECK(viUnlock(session) == VI_SUCCESS);

    CHECK(viLock(session, VI_SHARED_LOCK, 0, nullptr, key.data()) == VI_SUCCESS);
    CHECK(key[0] != '\0');
    CHECK(viLock(session, VI_SHARED_LOCK, 0, nullptr, nullptr) ==
          VI_SUCCESS_NESTED_SHARED);
    CHECK(viLock(peer, VI_SHARED_LOCK, 0, key.data(), nullptr) == VI_SUCCESS);
    CHECK(viUnlock(peer) == VI_SUCCESS);
    CHECK(viUnlock(session) == VI_SUCCESS);
    CHECK(viUnlock(session) == VI_SUCCESS);

    std::array<ViChar, VI_FIND_BUFLEN> description{};
    CHECK(viStatusDesc(rm, VI_ERROR_TMO, nullptr) == VI_ERROR_INV_PARAMETER);
    CHECK(viStatusDesc(VI_NULL, VI_ERROR_TMO, description.data()) ==
          VI_ERROR_INV_OBJECT);
    CHECK(viStatusDesc(rm, INT32_C(12345), description.data()) ==
          VI_ERROR_NSUP_OPER);
    CHECK(viStatusDesc(rm, VI_ERROR_TMO, description.data()) == VI_SUCCESS);
    CHECK(std::string(description.data()).find("timed out") != std::string::npos);

    CHECK(wrvisaSetSerialPath(rm, 1, nullptr) == VI_ERROR_INV_PARAMETER);
    CHECK(wrvisaSetSerialPath(session, 1, "/dev/null") ==
          VI_ERROR_INV_OBJECT);
    CHECK(wrvisaSetTcpipServicePort(rm, nullptr, WRVISA_TCPIP_PROTOCOL_VXI11,
                                    111) == VI_ERROR_INV_PARAMETER);
    CHECK(wrvisaSetTcpipServicePort(rm, "localhost", 99, 111) ==
          VI_ERROR_INV_PROT);
    CHECK(wrvisaSetTcpipServicePort(session, "localhost",
                                    WRVISA_TCPIP_PROTOCOL_HISLIP, 4880) ==
          VI_ERROR_INV_OBJECT);

    CHECK(viClose(peer) == VI_SUCCESS);
    CHECK(viClose(session) == VI_SUCCESS);
    CHECK(viClose(session) == VI_ERROR_INV_OBJECT);
    CHECK(viClose(rm) == VI_SUCCESS);
    return 0;
}
