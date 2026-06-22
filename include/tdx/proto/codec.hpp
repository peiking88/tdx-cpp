// 变长有符号整数解码 + 紧凑日期解码。
// 逐字对齐 opentdx utils/help.py:137-207（get_price / to_datetime）。
//
// get_price：类 UTF-8 变长有符号整数。
//   bit 0x40 = 符号位，bit 0x80 = 继续位。
//   首字节低 6 位（& 0x3f）；后续每字节低 7 位（& 0x7f）左移（从 bit6 起，每字节 +7）。
//
// to_datetime：紧凑日期。
//   with_time=false（日K）：num = YYYYMMDD 整数；越界回退紧凑编码。
//   with_time=true（分钟线）：低 16 位 = (year-2004)<<11 | mmdd，高 16 位 = 当日分钟数；越界回退 YYYYMMDD。
#pragma once

#include <cstddef>
#include <cstdint>

namespace tdx::proto {

// get_price 返回值：解码后的有符号整数 + 消费的下一位置。
struct PriceResult {
  int64_t value;
  std::size_t new_pos;
};

// 变长解码。对齐 help.py:137-169。pos >= data_len 时返回 (0, pos+1)。
PriceResult get_price(const uint8_t* data, std::size_t data_len, std::size_t pos);

// 紧凑日期 → epoch seconds（CST）。对齐 help.py:171-207（含双向 fallback）。
int64_t to_datetime(int64_t num, bool with_time);

}  // namespace tdx::proto
