#include <array>
#include <chrono>
#include <cstdlib>
#include <future>
#include <string>
#include <thread>

#include <fcntl.h>
#include <poll.h>
#include <unistd.h>

#include "core/handle_table.h"
#include "core/objects.h"
#include "test_support.h"
#include "visa.h"
#include "webreal_visa_ext.h"

namespace {

std::shared_ptr<wrvisa::SessionObject> session_object(ViSession session) {
    auto object = wrvisa::get_handle<wrvisa::SessionObject>(
        session, wrvisa::ObjectType::session);
    CHECK(object != nullptr);
    return object;
}

void wait_until_active(const std::shared_ptr<wrvisa::SessionObject>& session) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    while (session->active_operation_count() == 0 &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::yield();
    }
    CHECK(session->active_operation_count() != 0);
}

std::string read_master(int descriptor, std::size_t expected) {
    std::string result(expected, '\0');
    std::size_t offset = 0;
    while (offset < expected) {
        pollfd event{descriptor, POLLIN, 0};
        CHECK(poll(&event, 1, 1000) == 1);
        const auto amount = read(descriptor, result.data() + offset, expected - offset);
        CHECK(amount > 0);
        offset += static_cast<std::size_t>(amount);
    }
    return result;
}

}  // namespace

int main() {
    using namespace std::chrono_literals;

    const int master = posix_openpt(O_RDWR | O_NOCTTY);
    CHECK(master >= 0);
    CHECK(grantpt(master) == 0);
    CHECK(unlockpt(master) == 0);
    const char* slave = ptsname(master);
    CHECK(slave != nullptr);

    ViSession rm = VI_NULL;
    CHECK(viOpenDefaultRM(&rm) == VI_SUCCESS);
    CHECK(wrvisaSetSerialPath(rm, 77, slave) == VI_SUCCESS);
    CHECK(wrvisaSetSerialPath(rm, 78, "/wrvisa/does/not/exist") == VI_SUCCESS);
    ViSession missing = VI_NULL;
    CHECK(viOpen(rm, "ASRL78::INSTR", VI_NO_LOCK, 1000, &missing) ==
          VI_ERROR_RSRC_NFOUND);

    ViFindList list = VI_NULL;
    ViUInt32 found = 0;
    std::array<ViChar, VI_FIND_BUFLEN> found_name{};
    CHECK(viFindRsrc(rm, "ASRL77::INSTR", &list, &found, found_name.data()) ==
          VI_SUCCESS);
    CHECK(found == 1);
    CHECK(std::string(found_name.data()) == "ASRL77::INSTR");
    CHECK(viClose(list) == VI_SUCCESS);

    ViSession session = VI_NULL;
    CHECK(viOpen(rm, "ASRL77::INSTR", VI_NO_LOCK, 1000, &session) == VI_SUCCESS);
    ViUInt32 baud = 0;
    CHECK(viGetAttribute(session, VI_ATTR_ASRL_BAUD, &baud) == VI_SUCCESS);
    CHECK(baud == 9600);
    CHECK(viSetAttribute(session, VI_ATTR_ASRL_BAUD, 19200) == VI_SUCCESS);
    CHECK(viSetAttribute(session, VI_ATTR_ASRL_DATA_BITS, 8) == VI_SUCCESS);
    CHECK(viSetAttribute(session, VI_ATTR_ASRL_PARITY, VI_ASRL_PAR_NONE) == VI_SUCCESS);
    CHECK(viSetAttribute(session, VI_ATTR_ASRL_STOP_BITS, VI_ASRL_STOP_ONE) ==
          VI_SUCCESS);
    CHECK(viSetAttribute(session, VI_ATTR_ASRL_FLOW_CNTRL, VI_ASRL_FLOW_NONE) ==
          VI_SUCCESS);
    CHECK(viSetAttribute(session, VI_ATTR_ASRL_PARITY, VI_ASRL_PAR_MARK) ==
          VI_ERROR_NSUP_ATTR_STATE);

    const std::string request = "PING\n";
    ViUInt32 count = 0;
    CHECK(viWrite(session, reinterpret_cast<ViConstBuf>(request.data()),
                  static_cast<ViUInt32>(request.size()), &count) == VI_SUCCESS);
    CHECK(count == request.size());
    CHECK(read_master(master, request.size()) == request);

    const std::string response = "SERIAL-OK\n";
    CHECK(write(master, response.data(), response.size()) ==
          static_cast<ssize_t>(response.size()));
    CHECK(viSetAttribute(session, VI_ATTR_TERMCHAR, '\n') == VI_SUCCESS);
    CHECK(viSetAttribute(session, VI_ATTR_TERMCHAR_EN, VI_TRUE) == VI_SUCCESS);
    std::array<ViByte, 64> buffer{};
    CHECK(viRead(session, buffer.data(), static_cast<ViUInt32>(buffer.size()), &count) ==
          VI_SUCCESS_TERM_CHAR);
    CHECK(std::string(reinterpret_cast<char*>(buffer.data()), count) == response);

    CHECK(viSetAttribute(session, VI_ATTR_TMO_VALUE, 30) == VI_SUCCESS);
    count = 55;
    CHECK(viRead(session, buffer.data(), 3, &count) == VI_ERROR_TMO);
    CHECK(count == 0);

    CHECK(viSetAttribute(session, VI_ATTR_TMO_VALUE, 5000) == VI_SUCCESS);
    auto object = session_object(session);
    auto blocked = std::async(std::launch::async, [&] {
        return viRead(session, buffer.data(), 3, &count);
    });
    wait_until_active(object);
    CHECK(viTerminate(session, VI_NULL, VI_NULL) == VI_SUCCESS);
    CHECK(blocked.wait_for(1s) == std::future_status::ready);
    CHECK(blocked.get() == VI_ERROR_ABORT);
    CHECK(count == 0);

    const std::string after = "AFTER\n";
    CHECK(write(master, after.data(), after.size()) ==
          static_cast<ssize_t>(after.size()));
    CHECK(viRead(session, buffer.data(), static_cast<ViUInt32>(buffer.size()), &count) ==
          VI_SUCCESS_TERM_CHAR);
    CHECK(std::string(reinterpret_cast<char*>(buffer.data()), count) == after);

    CHECK(viFlush(session, VI_READ_BUF_DISCARD | VI_IO_IN_BUF_DISCARD) == VI_SUCCESS);
    CHECK(viClear(session) == VI_SUCCESS);
    CHECK(viClose(session) == VI_SUCCESS);
    CHECK(viClose(rm) == VI_SUCCESS);
    CHECK(close(master) == 0);
    return 0;
}
