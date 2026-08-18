// czsc 枚举类型：Direction, Mark, Freq
// 直译自:
//   ~/peiking88/czsc/crates/czsc-core/src/objects/direction.rs:29-36
//   ~/peiking88/czsc/crates/czsc-core/src/objects/mark.rs:31-38
//   ~/peiking88/czsc/crates/czsc-core/src/objects/freq.rs:40-155
#pragma once

#include <cstdint>
#include <string>

namespace czsc {

// 方向（direction.rs:29-36）
enum class Direction : uint8_t { kUp, kDown };

inline const char* DirectionName(Direction d) {
  switch (d) {
    case Direction::kUp:   return "Up";
    case Direction::kDown: return "Down";
  }
  return "Up";
}

inline const char* DirectionValue(Direction d) {
  switch (d) {
    case Direction::kUp:   return "\xe5\x90\x91\xe4\xb8\x8a";   // 向上
    case Direction::kDown: return "\xe5\x90\x91\xe4\xb8\x8b";   // 向下
  }
  return "";
}

// 分型类型（mark.rs:31-38）
enum class Mark : uint8_t { kD, kG };

inline const char* MarkName(Mark m) {
  switch (m) {
    case Mark::kD: return "D";
    case Mark::kG: return "G";
  }
  return "D";
}

inline const char* MarkValue(Mark m) {
  switch (m) {
    case Mark::kD: return "\xe5\xba\x95\xe5\x88\x86\xe5\x9e\x8b";   // 底分型
    case Mark::kG: return "\xe9\xa1\xb6\xe5\x88\x86\xe5\x9e\x8b";   // 顶分型
  }
  return "";
}

// 时间周期（freq.rs:40-155，21 个 variant）
enum class Freq : uint8_t {
  kTick, k1Min, k2Min, k3Min, k4Min, k5Min, k6Min,
  k10Min, k12Min, k15Min, k20Min, k30Min, k60Min,
  k120Min, k240Min, k360Min,
  kDay, kWeek, kMonth, kSeason, kYear
};

// 返回 Rust variant name（"F1", "F5", "D", "W"...）
inline const char* FreqName(Freq f) {
  switch (f) {
    case Freq::kTick:  return "Tick";
    case Freq::k1Min:  return "F1";
    case Freq::k2Min:  return "F2";
    case Freq::k3Min:  return "F3";
    case Freq::k4Min:  return "F4";
    case Freq::k5Min:  return "F5";
    case Freq::k6Min:  return "F6";
    case Freq::k10Min: return "F10";
    case Freq::k12Min: return "F12";
    case Freq::k15Min: return "F15";
    case Freq::k20Min: return "F20";
    case Freq::k30Min: return "F30";
    case Freq::k60Min: return "F60";
    case Freq::k120Min:return "F120";
    case Freq::k240Min:return "F240";
    case Freq::k360Min:return "F360";
    case Freq::kDay:   return "D";
    case Freq::kWeek:  return "W";
    case Freq::kMonth: return "M";
    case Freq::kSeason:return "S";
    case Freq::kYear:  return "Y";
  }
  return "D";
}

// 返回中文字符串（"1分钟", "日线"...）
inline const char* FreqValue(Freq f) {
  switch (f) {
    case Freq::kTick:  return "Tick";
    case Freq::k1Min:  return "1\xe5\x88\x86\xe9\x92\x9f";
    case Freq::k2Min:  return "2\xe5\x88\x86\xe9\x92\x9f";
    case Freq::k3Min:  return "3\xe5\x88\x86\xe9\x92\x9f";
    case Freq::k4Min:  return "4\xe5\x88\x86\xe9\x92\x9f";
    case Freq::k5Min:  return "5\xe5\x88\x86\xe9\x92\x9f";
    case Freq::k6Min:  return "6\xe5\x88\x86\xe9\x92\x9f";
    case Freq::k10Min: return "10\xe5\x88\x86\xe9\x92\x9f";
    case Freq::k12Min: return "12\xe5\x88\x86\xe9\x92\x9f";
    case Freq::k15Min: return "15\xe5\x88\x86\xe9\x92\x9f";
    case Freq::k20Min: return "20\xe5\x88\x86\xe9\x92\x9f";
    case Freq::k30Min: return "30\xe5\x88\x86\xe9\x92\x9f";
    case Freq::k60Min: return "60\xe5\x88\x86\xe9\x92\x9f";
    case Freq::k120Min:return "120\xe5\x88\x86\xe9\x92\x9f";
    case Freq::k240Min:return "240\xe5\x88\x86\xe9\x92\x9f";
    case Freq::k360Min:return "360\xe5\x88\x86\xe9\x92\x9f";
    case Freq::kDay:   return "\xe6\x97\xa5\xe7\xba\xbf";
    case Freq::kWeek:  return "\xe5\x91\xa8\xe7\xba\xbf";
    case Freq::kMonth: return "\xe6\x9c\x88\xe7\xba\xbf";
    case Freq::kSeason:return "\xe5\xad\xa3\xe7\xba\xbf";
    case Freq::kYear:  return "\xe5\xb9\xb4\xe7\xba\xbf";
  }
  return "";
}

// Freq 方法（freq.rs:112-155）
inline bool FreqIsMinute(Freq f) {
  switch (f) {
    case Freq::k1Min:  case Freq::k2Min:  case Freq::k3Min:
    case Freq::k4Min:  case Freq::k5Min:  case Freq::k6Min:
    case Freq::k10Min: case Freq::k12Min: case Freq::k15Min:
    case Freq::k20Min: case Freq::k30Min: case Freq::k60Min:
    case Freq::k120Min:case Freq::k240Min:case Freq::k360Min:
      return true;
    default:
      return false;
  }
}

inline int64_t FreqMinutes(Freq f) {
  switch (f) {
    case Freq::k1Min:  return 1;
    case Freq::k2Min:  return 2;
    case Freq::k3Min:  return 3;
    case Freq::k4Min:  return 4;
    case Freq::k5Min:  return 5;
    case Freq::k6Min:  return 6;
    case Freq::k10Min: return 10;
    case Freq::k12Min: return 12;
    case Freq::k15Min: return 15;
    case Freq::k20Min: return 20;
    case Freq::k30Min: return 30;
    case Freq::k60Min: return 60;
    case Freq::k120Min:return 120;
    case Freq::k240Min:return 240;
    case Freq::k360Min:return 360;
    default:           return -1;
  }
}

// Freq 从字符串解析（Rust EnumString trait 等价）
inline Freq FreqFromString(const std::string& s) {
  if (s == "Tick" || s == "逐笔") return Freq::kTick;
  if (s == "F1"  || s == "1分钟") return Freq::k1Min;
  if (s == "F2"  || s == "2分钟") return Freq::k2Min;
  if (s == "F3"  || s == "3分钟") return Freq::k3Min;
  if (s == "F4"  || s == "4分钟") return Freq::k4Min;
  if (s == "F5"  || s == "5分钟") return Freq::k5Min;
  if (s == "F6"  || s == "6分钟") return Freq::k6Min;
  if (s == "F10" || s == "10分钟") return Freq::k10Min;
  if (s == "F12" || s == "12分钟") return Freq::k12Min;
  if (s == "F15" || s == "15分钟") return Freq::k15Min;
  if (s == "F20" || s == "20分钟") return Freq::k20Min;
  if (s == "F30" || s == "30分钟") return Freq::k30Min;
  if (s == "F60" || s == "60分钟") return Freq::k60Min;
  if (s == "F120"|| s == "120分钟") return Freq::k120Min;
  if (s == "F240"|| s == "240分钟") return Freq::k240Min;
  if (s == "F360"|| s == "360分钟") return Freq::k360Min;
  if (s == "D"   || s == "日线") return Freq::kDay;
  if (s == "W"   || s == "周线" || s == "week") return Freq::kWeek;
  if (s == "M"   || s == "月线") return Freq::kMonth;
  if (s == "S"   || s == "季线") return Freq::kSeason;
  if (s == "Y"   || s == "年线") return Freq::kYear;
  return Freq::kDay;  // 默认日线
}

}  // namespace czsc
