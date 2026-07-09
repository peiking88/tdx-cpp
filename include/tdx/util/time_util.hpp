// 时间工具。A 股时间为 CST（UTC+8）。所有 datetime 字段统一存 epoch seconds
// （CST 时刻对应的绝对 UTC epoch），消除 Dual Schema（评审 P2-4）。
//
// 实现：内部用 abseil Time + FixedTimeZone(+8) 替代手写 timegm/gmtime_r 偏移，
// 线程安全、无 POSIX 非标准依赖，为跨时区扩展铺路。
#pragma once

#include <ctime>
#include <cstdint>

#include "tdx/consts.hpp"

namespace tdx::util {

// CST 时刻（年月日时分）→ epoch seconds。
int64_t cst_to_epoch(int year, int month, int day, int hour, int minute);

// 日 K（仅有日期）→ epoch，时间锚定 15:00 CST 收盘（对齐 opentdx to_datetime 默认）。
int64_t date_to_epoch(int year, int month, int day);

// epoch → CST 民用时刻（用于格式化/校验）
struct CivilTime {
  int year;
  int month;
  int day;
  int hour;
  int minute;
  int second;
};
CivilTime epoch_to_cst(int64_t epoch);

// 当日分钟数（0:00 起）→ hour/minute
inline void minutes_to_hm(int minutes, int& hour, int& minute) {
  hour = minutes / 60;
  minute = minutes % 60;
}

// 单根 K 线是否在 A 股交易时段内（用于 parser/cli 拦截脏数据）。
// 1d 锚定 15:00 收盘；1m/5m 须在早盘 9:30–11:30 或午盘 13:00–15:00。
// 对齐 opentdx：A 股交易时段不含集合竞价，15:00 收盘最后一根有效。
// 入参 datetime 为 epoch seconds（CST）。
inline bool IsBarInTradingHours(int64_t datetime, Period period) {
  auto ci = epoch_to_cst(datetime);
  int mins = ci.hour * 60 + ci.minute;
  if (period == Period::DAILY) {
    // 日 K 锚定 15:00 收盘。
    return ci.hour == 15 && ci.minute == 0;
  }
  // 1m / 5m：早盘 570–690（9:30–11:30），午盘 780–900（13:00–15:00）。
  return (mins >= 570 && mins <= 690) || (mins >= 780 && mins <= 900);
}

// 今日 00:00 CST epoch seconds（用于 sync-kline 仅保留当日 bar）。
inline int64_t TodayMidnightEpoch() {
  std::time_t now = std::time(nullptr);
  std::tm tm{};
  localtime_r(&now, &tm);
  tm.tm_hour = 0;
  tm.tm_min = 0;
  tm.tm_sec = 0;
  return static_cast<int64_t>(std::mktime(&tm));
}

}  // namespace tdx::util
