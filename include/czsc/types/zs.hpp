// ZS：中枢
// 直译自 ~/peiking88/czsc/crates/czsc-core/src/objects/zs.rs:21-100
#pragma once

#include <cstdint>
#include <vector>

#include <nlohmann/json_fwd.hpp>

#include "czsc/types/bi.hpp"

namespace czsc {

struct ZS {
  std::vector<BI> bis;
  int64_t sdt = 0;                         // 中枢开始时间
  int64_t edt = 0;                         // 中枢结束时间
  Direction sdir = Direction::kUp;          // 第一笔方向
  Direction edir = Direction::kUp;          // 最后一笔方向
  double zg = 0.0;                         // 中枢上沿（zs.rs:31）
  double zd = 0.0;                         // 中枢下沿（zs.rs:33）
  double zz = 0.0;                         // 中枢中轴（zs.rs:35）
  double gg = 0.0;                         // 中枢最高点（zs.rs:37）
  double dd = 0.0;                         // 中枢最低点（zs.rs:39）

  ZS() = default;

  // 从笔列表构造中枢（zs.rs:47-87 new）
  explicit ZS(const std::vector<BI>& bis);

  // 中枢是否有效（zs.rs:90-100）
  bool is_valid() const noexcept;
};

bool operator==(const ZS& a, const ZS& b) noexcept;
inline bool operator!=(const ZS& a, const ZS& b) noexcept { return !(a == b); }

void to_json(nlohmann::json& j, const ZS& zs);
void from_json(const nlohmann::json& j, ZS& zs);

}  // namespace czsc
