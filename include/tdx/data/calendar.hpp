// 交易日历。对齐 tdxdata/calendar.py（is_trading_day/get_trading_days）。
// tdxdata 委托 mootdx 网络爬虫（PRD Won't HTTP），C++ 改用硬编码 cfg/holidays.json。
#pragma once

#include <string>
#include <unordered_set>
#include <vector>

namespace tdx::data {

class Calendar {
 public:
  // 默认读 cfg/holidays.json（相对 cwd）
  Calendar();
  explicit Calendar(std::string holidays_path);

  // date 格式 "YYYY-MM-DD"
  bool IsTradingDay(const std::string& date) const;
  bool IsTradingDay(int year, int month, int day) const;

  // 枚举 [start, end] 内交易日（逐日遍历，对齐 calendar.py:55-61）
  std::vector<std::string> GetTradingDays(const std::string& start,
                                          const std::string& end) const;

  void LoadHolidays(const std::string& path);
  bool IsHoliday(const std::string& date) const;

 private:
  std::unordered_set<std::string> holidays_;  // ponytail: O(1) lookup, vector→set
};

}  // namespace tdx::data
