#include "tdx/proto/codec.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
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
  int now_year = current_year();

  if (with_time) {
    int64_t zip_data = num & 0xFFFF;
    year = static_cast<int>((zip_data >> 11) + 2004);
    month = static_cast<int>((zip_data & 0x7FF) / 100);
    day = static_cast<int>((zip_data & 0x7FF) % 100);

    int64_t minutes = num >> 16;
    hour = static_cast<int>(minutes / 60);
    minute = static_cast<int>(minutes % 60);

    // 越界回退 YYYYMMDD（对齐 help.py:186-191）
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

// server_time（HHMMSSmm 整数，前导0丢失）→ 当日 CST epoch。复刻 help.py:209 format_time。
int64_t format_time_to_epoch(int64_t ts) {
  if (ts <= 0 || ts == 100) return 0;  // 服务器未返回时间 → 调用方用 now 兜底
  char buf[24];
  int L = std::snprintf(buf, sizeof(buf), "%lld", static_cast<long long>(ts));
  if (L < 7) return 0;  // HHMMSSmm 至少 7 位（如 9301500 = 09:30:15）
  auto slice = [&](int from, int len) -> long long {
    int start = from < 0 ? 0 : from;
    if (start >= L) return 0;
    int cnt = (len < L - start) ? len : (L - start);
    char tmp[16];
    std::memcpy(tmp, buf + start, cnt);
    tmp[cnt] = '\0';
    return std::atoll(tmp);
  };
  int hh = static_cast<int>(slice(0, L - 6));
  int mm_check = static_cast<int>(slice(L - 6, 2));
  int mm, ss;
  if (mm_check < 60) {
    mm = mm_check;
    ss = static_cast<int>(slice(L - 4, 4) * 60 / 10000);
  } else {
    long long tail6 = slice(L - 6, 6);
    mm = static_cast<int>(tail6 * 60 / 1000000);
    ss = static_cast<int>((tail6 * 60 % 1000000) * 60 / 1000000);
  }
  if (hh > 23 || mm > 59 || ss > 59) return 0;
  auto c = util::epoch_to_cst(std::time(nullptr));  // 当日 CST 日期
  return util::cst_to_epoch(c.year, c.month, c.day, hh, mm);
}

}  // namespace tdx::proto
