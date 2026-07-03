// 通达信帧编解码。逐字对齐 opentdx parser/baseParser.py:14-20（封包）
// 与 client/baseStockClient.py:299-321（解包）。
//
// 请求布局（全小端）：
//   header <BIBHH>  head(B) customize(I) control=1(B) lbody(H) lbody(H)   —— 10 字节
//   body            msg_id(H) payload                                      —— lbody = 2 + len(payload)
//   前 12 字节即 <BIBHHH>：head, customize, control, lbody, lbody, msg_id
//
// 响应头 <IBIBHHH>（16 字节，packed 无对齐填充）：
//   prefix(I=0x0074cbb1, 即 b1 cb 74 00) zipped(B) customize(I) unknown(B)
//   msg_id(H) zipsize(H) unzip_size(H)
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "tdx/errors.hpp"

namespace tdx::proto {

// head 字节：0x0c 不压缩，0x1c 请求压缩（仅当 head==0x0c 且 need_zip 时升级）
inline constexpr uint8_t kHeadNoZip = 0x0c;
inline constexpr uint8_t kHeadZip = 0x1c;

// 响应头固定 prefix（小端 4 字节 b1 cb 74 00）
inline constexpr uint32_t kRspPrefix = 0x0074cbb1;

inline constexpr std::size_t kReqHeaderLen = 10;   // 请求 header 字节数（不含 body）
inline constexpr std::size_t kRspHeaderLen = 16;   // 响应头字节数（RSP_HEADER_LEN）

// 封包请求。对齐 baseParser.py serialize。
std::vector<uint8_t> pack_request(uint8_t head, uint32_t customize, uint16_t msg_id,
                                  const uint8_t* payload, std::size_t payload_len,
                                  bool need_zip = false);

// 响应头（对齐 baseStockClient.py:307 <IBIBHHH>）
struct ResponseHeader {
  uint32_t prefix;
  uint8_t zipped;
  uint32_t customize;
  uint8_t unknown;
  uint16_t msg_id;
  uint16_t zip_size;
  uint16_t unzip_size;
};

// 解析 16 字节响应头，校验 prefix；prefix 不符抛 TdxProtocolError。
ResponseHeader parse_response_header(const uint8_t* buf16);

// body 是否需要 zlib 解压（zipsize != unzip_size）
inline constexpr bool need_unzip(const ResponseHeader& h) { return h.zip_size != h.unzip_size; }

}  // namespace tdx::proto
