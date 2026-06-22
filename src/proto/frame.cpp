#include "tdx/proto/frame.hpp"

#include "tdx/util/byte_order.hpp"

namespace tdx::proto {

std::vector<uint8_t> pack_request(uint8_t head, uint32_t customize, uint16_t msg_id,
                                  const uint8_t* payload, std::size_t payload_len,
                                  bool need_zip) {
  // 对齐 baseParser.py:14-20：head==0x0c 且 need_zip 时升级为 0x1c。
  uint8_t h = (head == kHeadNoZip && need_zip) ? kHeadZip : head;
  uint16_t lbody = static_cast<uint16_t>(2 + payload_len);  // body = msg_id(2) + payload

  std::vector<uint8_t> out;
  out.reserve(kReqHeaderLen + 2 + payload_len);

  // header <BIBHH>：head, customize, control=1, lbody, lbody
  auto push_u8 = [&](uint8_t v) { out.push_back(v); };
  auto push_u16 = [&](uint16_t v) {
    out.push_back(static_cast<uint8_t>(v & 0xff));
    out.push_back(static_cast<uint8_t>((v >> 8) & 0xff));
  };
  auto push_u32 = [&](uint32_t v) {
    for (int i = 0; i < 4; ++i) out.push_back(static_cast<uint8_t>((v >> (8 * i)) & 0xff));
  };

  push_u8(h);
  push_u32(customize);
  push_u8(1);  // control 固定 1
  push_u16(lbody);
  push_u16(lbody);

  // body：msg_id(H) + payload
  push_u16(msg_id);
  if (payload_len > 0) {
    out.insert(out.end(), payload, payload + payload_len);
  }
  return out;
}

ResponseHeader parse_response_header(const uint8_t* buf) {
  ResponseHeader h{};
  // packed 偏移：<IBIBHHH> → I@0 B@4 I@5 B@9 H@10 H@12 H@14
  h.prefix = util::rd_u32(buf);
  if (h.prefix != kRspPrefix) {
    throw TdxProtocolError("invalid response prefix (expected b1 cb 74 00)");
  }
  h.zipped = util::rd_u8(buf + 4);
  h.customize = util::rd_u32(buf + 5);
  h.unknown = util::rd_u8(buf + 9);
  h.msg_id = util::rd_u16(buf + 10);
  h.zip_size = util::rd_u16(buf + 12);
  h.unzip_size = util::rd_u16(buf + 14);
  return h;
}

}  // namespace tdx::proto
