#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "backends/hislip/hislip_protocol.h"
#include "backends/vxi11/vxi11_protocol.h"
#include "test_support.h"

int main() {
    using namespace wrvisa;

    const std::array<std::uint8_t, 3> opaque{{0x11, 0x22, 0x33}};
    vxi11::XdrWriter writer;
    writer.u32(UINT32_C(0x01020304));
    writer.boolean(true);
    writer.opaque(opaque);
    const std::array<std::uint8_t, 16> expected_xdr{{
        0x01, 0x02, 0x03, 0x04, 0x00, 0x00, 0x00, 0x01,
        0x00, 0x00, 0x00, 0x03, 0x11, 0x22, 0x33, 0x00,
    }};
    CHECK(writer.bytes() ==
          std::vector<std::uint8_t>(expected_xdr.begin(), expected_xdr.end()));

    vxi11::XdrReader reader(writer.bytes());
    std::uint32_t value = 0;
    std::vector<std::uint8_t> decoded_opaque;
    CHECK(reader.u32(value));
    CHECK(value == UINT32_C(0x01020304));
    CHECK(reader.u32(value));
    CHECK(value == 1);
    CHECK(reader.opaque(decoded_opaque, 3));
    CHECK(decoded_opaque ==
          std::vector<std::uint8_t>(opaque.begin(), opaque.end()));
    CHECK(reader.remaining().empty());

    std::uint32_t truncated_value = 0;
    const std::array<std::uint8_t, 3> truncated_u32{{0, 0, 0}};
    vxi11::XdrReader truncated_reader(truncated_u32);
    CHECK(!truncated_reader.u32(truncated_value));

    const std::array<std::uint8_t, 8> oversized_opaque{{
        0, 0, 0, 5, 1, 2, 3, 4,
    }};
    std::vector<std::uint8_t> rejected_opaque;
    vxi11::XdrReader oversized_reader(oversized_opaque);
    CHECK(!oversized_reader.opaque(rejected_opaque, 4));
    vxi11::XdrReader truncated_opaque_reader(oversized_opaque);
    CHECK(!truncated_opaque_reader.opaque(rejected_opaque, 8));

    const auto rpc = vxi11::make_rpc_call(
        UINT32_C(0x12345678), vxi11::kDeviceCoreProgram,
        vxi11::kProgramVersion, vxi11::kCreateLink, writer.bytes());
    CHECK(rpc.size() == 4u + 40u + writer.bytes().size());
    CHECK(rpc[0] == 0x80);
    CHECK(rpc[4] == 0x12 && rpc[5] == 0x34 && rpc[6] == 0x56 && rpc[7] == 0x78);

    const std::array<std::uint8_t, 28> accepted_reply{{
        0x12, 0x34, 0x56, 0x78,  // XID
        0, 0, 0, 1,              // REPLY
        0, 0, 0, 0,              // MSG_ACCEPTED
        0, 0, 0, 0,              // AUTH_NONE
        0, 0, 0, 0,              // verifier length
        0, 0, 0, 0,              // SUCCESS
        0xCA, 0xFE, 0xBA, 0xBE,  // result
    }};
    std::span<const std::uint8_t> rpc_result;
    CHECK(vxi11::parse_rpc_reply(accepted_reply, UINT32_C(0x12345678),
                                 rpc_result));
    CHECK(rpc_result.size() == 4);
    CHECK(rpc_result[0] == 0xCA && rpc_result[3] == 0xBE);
    CHECK(!vxi11::parse_rpc_reply(accepted_reply, UINT32_C(0x87654321),
                                  rpc_result));
    CHECK(!vxi11::parse_rpc_reply(
        std::span<const std::uint8_t>(accepted_reply).first(19),
        UINT32_C(0x12345678), rpc_result));
    auto rejected_reply = accepted_reply;
    rejected_reply[23] = 1;  // accepted status is not SUCCESS
    CHECK(!vxi11::parse_rpc_reply(rejected_reply, UINT32_C(0x12345678),
                                  rpc_result));

    const std::string payload = "abc";
    const auto encoded = hislip::encode(
        hislip::MessageType::data_end, 1, UINT32_C(0x01020304),
        std::span<const std::uint8_t>(
            reinterpret_cast<const std::uint8_t*>(payload.data()), payload.size()));
    const std::array<std::uint8_t, 16> expected_header{{
        'H', 'S', 7, 1, 1, 2, 3, 4, 0, 0, 0, 0, 0, 0, 0, 3,
    }};
    CHECK(std::equal(expected_header.begin(), expected_header.end(),
                     encoded.begin()));
    hislip::Frame frame;
    CHECK(hislip::decode(encoded, frame));
    CHECK(frame.type == hislip::MessageType::data_end);
    CHECK(frame.control == 1);
    CHECK(frame.parameter == UINT32_C(0x01020304));
    CHECK(std::string(frame.payload.begin(), frame.payload.end()) == "abc");

    auto invalid = encoded;
    invalid[0] = 'X';
    CHECK(!hislip::decode(invalid, frame));
    invalid = encoded;
    invalid[15] = 4;
    CHECK(!hislip::decode(invalid, frame));
    CHECK(!hislip::decode(std::span<const std::uint8_t>(encoded).first(15), frame));
    invalid = encoded;
    invalid[8] = 0x80;
    CHECK(!hislip::decode(invalid, frame));
    return 0;
}
