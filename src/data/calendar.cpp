// Calendar 实现。对齐 tdxdata/calendar.py。节假日读 cfg/holidays.json（替代 mootdx 爬虫）。
#include "tdx/data/calendar.hpp"

#include <cstdio>
#include <ctime>
#include <fstream>

#include <nlohmann/json.hpp>

namespace tdx::data {
namespace {

bool IsWeekend(int year, int month, int day) {
  std::tm t{};
  t.tm_year = year - 1900;
  t.tm_mon = month - 1;
  t.tm_mday = day;
  t.tm_isdst = -1;
  std::time_t tt = std::mktime(&t);
  std::tm lt{};
  localtime_r(&tt, &lt);
  return lt.tm_wday == 0 || lt.tm_wday == 6;  // 周日/周六
}

std::string DateStr(int y, int m, int d) {
  char b[32];  // ponytail: snprintf 溢出告警，16→32
  std::snprintf(b, sizeof(b), "%04d-%02d-%02d", y, m, d);
  return std::string(b);
}

}  // namespace

Calendar::Calendar() { LoadHolidays("cfg/holidays.json"); }

Calendar::Calendar(std::string holidays_path) { LoadHolidays(std::move(holidays_path)); }

void Calendar::LoadHolidays(const std::string& path) {
  std::ifstream f(path);
  if (!f) return;
  try {
    auto j = nlohmann::json::parse(f);
    if (j.is_array()) {
      for (const auto& d : j) holidays_.insert(d.get<std::string>());  // ponytail: unordered_set
    }
  } catch (...) {
    // 节假日文件解析失败，静默降级为「仅周末非交易日」
  }
}

bool Calendar::IsHoliday(const std::string& date) const {
  return holidays_.count(date) > 0;  // ponytail: O(1)
}

bool Calendar::IsTradingDay(const std::string& date) const {
  if (date.size() < 10) return false;
  int y = std::stoi(date.substr(0, 4));
  int m = std::stoi(date.substr(5, 2));
  int d = std::stoi(date.substr(8, 2));
  return IsTradingDay(y, m, d);
}

bool Calendar::IsTradingDay(int year, int month, int day) const {
  if (IsWeekend(year, month, day)) return false;
  return !IsHoliday(DateStr(year, month, day));
}

std::vector<std::string> Calendar::GetTradingDays(const std::string& start,
                                                  const std::string& end) const {
  std::vector<std::string> days;
  if (start.size() < 10 || end.size() < 10) return days;
  std::tm t{};
  t.tm_year = std::stoi(start.substr(0, 4)) - 1900;
  t.tm_mon = std::stoi(start.substr(5, 2)) - 1;
  t.tm_mday = std::stoi(start.substr(8, 2));
  t.tm_isdst = -1;
  std::tm end_t{};
  end_t.tm_year = std::stoi(end.substr(0, 4)) - 1900;
  end_t.tm_mon = std::stoi(end.substr(5, 2)) - 1;
  end_t.tm_mday = std::stoi(end.substr(8, 2));
  end_t.tm_isdst = -1;
  std::time_t end_tt = std::mktime(&end_t);

  while (true) {
    std::time_t tt = std::mktime(&t);
    if (tt > end_tt) break;
    if (IsTradingDay(t.tm_year + 1900, t.tm_mon + 1, t.tm_mday)) {
      days.push_back(DateStr(t.tm_year + 1900, t.tm_mon + 1, t.tm_mday));
    }
    t.tm_mday++;
    t.tm_isdst = -1;
    std::mktime(&t);  // 规范化（自动进位月/年）
  }
  return days;
}

}  // namespace tdx::data
