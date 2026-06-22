// 本地 vipdoc 文件读取器。逐字对齐 opentdx/reader/daily_bar_reader.py（.day）
// 与 lc_min_bar_reader.py（.lc1/.lc5）。不在 fiber 内（同步文件 IO）。
//
// 格式（逐字移植，注意与部分文档表述不同，以 opentdx 源码为准）：
//   .day  日线 <IIIIIfII> 32B：date(I YYYYMMDD) open(I) high(I) low(I) close(I) amount(f) volume(I) reserved(I)
//         价格/量为整数，A股需 ×0.01 缩放（opentdx SECURITY_COEFFICIENT）
//   .lc1/.lc5 分钟 <HHfffffII> 32B：date(H 紧凑) time(H 紧凑) open(f) high(f) low(f) close(f) amount(f) volume(I) reserved(I)
#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "tdx/consts.hpp"
#include "tdx/types.hpp"

namespace tdx::proto {

class VipdocReader {
 public:
  // 默认读环境变量 TDX_HOME；为空时需显式 set_tdx_home。
  VipdocReader();
  explicit VipdocReader(std::string tdx_home);

  // 读 .day 日线（价格/量 ×0.01 缩放）。
  std::vector<KLine> ReadDay(Market market, std::string_view code) const;
  // 读 .lc5 5 分钟线。
  std::vector<KLine> ReadMin5(Market market, std::string_view code) const;
  // 读 .lc1 1 分钟线。
  std::vector<KLine> ReadMin1(Market market, std::string_view code) const;

  void set_tdx_home(std::string home) { tdx_home_ = std::move(home); }
  const std::string& tdx_home() const { return tdx_home_; }

 private:
  static std::string MarketDir(Market m);  // sh/sz/bj
  std::string FilePath(Market m, std::string_view code,
                       std::string_view subdir, std::string_view ext) const;
  std::vector<KLine> ReadMin(Market market, std::string_view code,
                             std::string_view subdir, std::string_view ext) const;

  std::string tdx_home_;
};

}  // namespace tdx::proto
