#include "tdx/proto/codec.hpp"

#include <ctime>

#include "tdx/util/time_util.hpp"

namespace tdx::proto {

PriceResult get_price(const uint8_t* data, std::size_t data_len, std::size_t pos) {
  if (pos >= data_len) return {0, pos + 1};

  std::size_t pos_byte = 6;
  uint8_t bdata = data[pos];
  int64_t int_data = bdata & 0x3f;
  bool sign = (bdata & 0x40) != 0;

  if (bdata & 0x80) {
    while (true) {
      ++pos;
      if (pos >= data_len) break;
      bdata = data[pos];
      int_data += static_cast<int64_t>(bdata & 0x7f) << pos_byte;
      pos_byte += 7;
      if (!(bdata & 0x80)) break;
    }
  }
  ++pos;

  if (sign) int_data = -int_data;
  return {int_data, pos};
}

namespace {
// 对齐 help.py:186 的 datetime.now().year 上限判断。
int current_year() {
  std::time_t t = std::time(nullptr);
  std::tm tm{};
  localtime_r(&t, &tm);
  return tm.tm_year + 1900;
}
}  // namespace

int64_t to_datetime(int64_t num, bool with_time) {
  int year = 0, month = 0, day = 0, hour = 15, minute = 0;

  if (with_time) {
    int64_t zip_data = num & 0xFFFF;
    year = static_cast<int>((zip_data >> 11) + 2004);
    month = static_cast<int>((zip_data & 0x7FF) / 100);
    day = static_cast<int>((zip_data & 0x7FF) % 100);

    int64_t minutes = num >> 16;
    hour = static_cast<int>(minutes / 60);
    minute = static_cast<int>(minutes % 60);

    // 越界回退 YYYYMMDD（对齐 help.py:186-191）
    int now_year = current_year();
    if (month < 1 || month > 12 || day < 1 || day > 31 || year > now_year ||
        hour > 23 || minute > 59) {
      year = static_cast<int>(num / 10000);
      month = static_cast<int>(num % 10000 / 100);
      day = static_cast<int>(num % 100);
      hour = 15;
      minute = 0;
    }
  } else {
    year = static_cast<int>(num / 10000);
    month = static_cast<int>(num % 10000 / 100);
    day = static_cast<int>(num % 100);

    // 越界回退紧凑编码（对齐 help.py:196-202）
    if (year > 2100 || month < 1 || month > 12 || day < 1 || day > 31) {
      int64_t zip_data = num & 0xFFFF;
      year = static_cast<int>((zip_data >> 11) + 2004);
      month = static_cast<int>((zip_data & 0x7FF) / 100);
      day = static_cast<int>((zip_data & 0x7FF) % 100);
      hour = 15;
      minute = 0;
    }
  }

  // 非法日期兜底（对齐 help.py:203-207 try/except）。正常数据不触发。
  if (month < 1 || month > 12 || day < 1 || day > 31) return 0;

  return util::cst_to_epoch(year, month, day, hour, minute);
}

}  // namespace tdx::proto
