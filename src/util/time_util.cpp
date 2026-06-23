// time_util 实现。内部用 absl::Time + FixedTimeZone(+8) 替代手写 timegm/gmtime_r。
#include "tdx/util/time_util.hpp"

#include "absl/time/civil_time.h"
#include "absl/time/time.h"

namespace tdx::util {
namespace {

// CST = UTC+8，FixedTimeZone 不依赖 IANA tzdata，构造函数 constexpr-safe。
absl::TimeZone CstZone() {
  return absl::FixedTimeZone(8 * 60 * 60);  // +08:00
}

}  // namespace

int64_t cst_to_epoch(int year, int month, int day, int hour, int minute) {
  auto t = absl::FromCivil(absl::CivilSecond(year, month, day, hour, minute, 0), CstZone());
  return absl::ToUnixSeconds(t);
}

int64_t date_to_epoch(int year, int month, int day) {
  return cst_to_epoch(year, month, day, 15, 0);  // 日 K 锚定 15:00 收盘
}

CivilTime epoch_to_cst(int64_t epoch) {
  auto t = absl::FromUnixSeconds(epoch);
  auto ci = CstZone().At(t);
  return {ci.cs.year(), ci.cs.month(), ci.cs.day(),
          ci.cs.hour(), ci.cs.minute(), ci.cs.second()};
}

}  // namespace tdx::util
