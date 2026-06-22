#include "tdx/util/time_util.hpp"

#include <ctime>

namespace tdx::util {

int64_t cst_to_epoch(int year, int month, int day, int hour, int minute) {
  std::tm t{};
  t.tm_year = year - 1900;
  t.tm_mon = month - 1;
  t.tm_mday = day;
  t.tm_hour = hour;
  t.tm_min = minute;
  t.tm_sec = 0;
  t.tm_isdst = 0;
  // timegm 把 tm 当 UTC 解释；CST = UTC+8，故 CST 时刻的 UTC epoch = timegm - 8h。
  time_t tt = timegm(&t);
  return static_cast<int64_t>(tt) - 8 * 3600;
}

int64_t date_to_epoch(int year, int month, int day) {
  return cst_to_epoch(year, month, day, 15, 0);  // 日 K 锚定 15:00 收盘
}

CivilTime epoch_to_cst(int64_t epoch) {
  time_t tt = static_cast<time_t>(epoch) + 8 * 3600;  // UTC epoch → CST 时刻
  std::tm t{};
  gmtime_r(&tt, &t);
  return {t.tm_year + 1900, t.tm_mon + 1, t.tm_mday, t.tm_hour, t.tm_min, t.tm_sec};
}

}  // namespace tdx::util
