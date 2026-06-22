// 时间工具。A 股时间为 CST（UTC+8）。所有 datetime 字段统一存 epoch seconds
// （CST 时刻对应的绝对 UTC epoch），消除 Dual Schema（评审 P2-4）。
#pragma once

#include <cstdint>

namespace tdx::util {

// CST 时刻（年月日时分）→ epoch seconds。
// 实现把该时刻当 UTC 解释（timegm）再减 8h，得到 CST 时刻的绝对 UTC epoch。
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

}  // namespace tdx::util
