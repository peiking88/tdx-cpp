// frame 单元测试：请求封包 + 响应解包 + prefix 校验 + 解压判定（D7/D8）。
// 对齐 opentdx baseParser.py:14-20 + baseStockClient.py:299-321。
#include "tdx/proto/frame.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

#include "tdx/errors.hpp"

using namespace tdx::proto;

TEST(PackRequest, BasicLayout) {
  uint8_t payload[] = {0x01, 0x02};
  auto pkt = pack_request(0x0c, 0, 0x523, payload, 2, false);
  // header <BIBHH>：head, customize(4 LE), control=1, lbody(LE), lbody(LE)
  // body：msg_id(H LE) + payload；lbody = 2 + 2 = 4
  std::vector<uint8_t> expected = {
      0x0c,                       // head
      0x00, 0x00, 0x00, 0x00,     // customize=0
      0x01,                       // control=1
      0x04, 0x00,                 // lbody=4
      0x04, 0x00,                 // lbody=4
      0x23, 0x05,                 // msg_id=0x523
      0x01, 0x02                  // payload
  };
  EXPECT_EQ(pkt, expected);
}

TEST(PackRequest, NeedZipUpgradesHead) {
  auto pkt = pack_request(0x0c, 0, 0x04, nullptr, 0, true);
  EXPECT_EQ(pkt[0], 0x1c);  // head 升级为压缩
}

TEST(PackRequest, EmptyPayload) {
  auto pkt = pack_request(0x0c, 0x04, 0x04, nullptr, 0, false);
  // lbody = 2（仅 msg_id）
  ASSERT_GE(pkt.size(), 10u);
  EXPECT_EQ(pkt[6], 0x02);  // lbyte 低字节 = 2
  EXPECT_EQ(pkt[8], 0x02);
  EXPECT_EQ(pkt.size(), 12u);  // 10 header + 2 msg_id
}

TEST(ParseResponseHeader, Valid) {
  uint8_t buf[16] = {
      0xb1, 0xcb, 0x74, 0x00,   // prefix = 0x0074cbb1
      0x0c,                      // zipped（不压缩）
      0x00, 0x00, 0x00, 0x00,   // customize=0
      0x00,                      // unknown
      0x23, 0x05,                // msg_id=0x523
      0x0a, 0x00,                // zipsize=10
      0x14, 0x00                 // unzipsize=20
  };
  auto h = parse_response_header(buf);
  EXPECT_EQ(h.prefix, 0x0074cbb1u);
  EXPECT_EQ(h.zipped, 0x0c);
  EXPECT_EQ(h.customize, 0x00000000u);
  EXPECT_EQ(h.msg_id, 0x523);
  EXPECT_EQ(h.zip_size, 10);
  EXPECT_EQ(h.unzip_size, 20);
  EXPECT_TRUE(need_unzip(h));
}

TEST(ParseResponseHeader, NoUnzipWhenEqual) {
  uint8_t buf[16] = {
      0xb1, 0xcb, 0x74, 0x00, 0x0c,
      0x00, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x00,  // msg_id
      0x14, 0x00,  // zipsize=20
      0x14, 0x00   // unzipsize=20（相等 → 不解压）
  };
  auto h = parse_response_header(buf);
  EXPECT_FALSE(need_unzip(h));
}

TEST(ParseResponseHeader, InvalidPrefix) {
  uint8_t buf[16] = {0};  // prefix 全 0，错误
  EXPECT_THROW(parse_response_header(buf), tdx::TdxProtocolError);
}
