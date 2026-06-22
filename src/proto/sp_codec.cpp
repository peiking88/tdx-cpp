#include "tdx/proto/sp_codec.hpp"

#include <cstdlib>
#include <string>

#include "tdx/util/time_util.hpp"

namespace tdx::proto {

int64_t combine_to_datetime(int ymd, int date_num, bool format_tdx_time) {
  // 对齐 help.py:55-63：ymd 是 YYYYMMDD 整数，date_num 是当日秒数。
  int year = ymd / 10000;
  int month = (ymd % 10000) / 100;
  int day = ymd % 100;
  int hours = date_num / 3600;
  int minutes = (date_num % 3600) / 60;
  int64_t epoch = util::cst_to_epoch(year, month, day, hours, minutes);
  // 美股/期货日期偏移：0-5 点的视为次日（help.py:61-62）
  if (format_tdx_time && hours >= 0 && hours <= 5) {
    epoch += 24 * 3600;
  }
  return epoch;
}

int exchange_board_code(std::string_view board_symbol) {
  // 对齐 help.py:66-89。
  std::string s(board_symbol);
  if (s.rfind("US", 0) == 0) {
    return 30000 + std::atoi(s.c_str() + 2);
  }
  if (s.rfind("HK", 0) == 0) {
    return 20000 + std::atoi(s.c_str() + 2);
  }
  if (s.rfind("000", 0) == 0) {
    return 31000 + std::atoi(s.c_str());
  }
  if (s.rfind("399", 0) == 0 && s.size() == 6) {
    return std::atoi(s.c_str()) - 399000 + 30000;
  }
  if (s.rfind("899", 0) == 0 && s.size() == 6) {
    return std::atoi(s.c_str()) - 899000 + 32000;
  }
  if (s.rfind("88", 0) == 0 && s.size() == 6) {
    return std::atoi(s.c_str()) - 880000 + 20000;
  }
  return std::atoi(s.c_str());
}

}  // namespace tdx::proto
