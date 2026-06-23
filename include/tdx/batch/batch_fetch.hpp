// 并发批量拉取。N worker fiber 分片（每 worker 独立 Connection，非线程安全）。
// -n 控制并发数（worker 数 = Connection 数）。替代 tdxdata 纯串行 base.py:184。
#pragma once

#include <string>
#include <vector>

#include "tdx/consts.hpp"
#include "tdx/types.hpp"

namespace tdx::batch {

struct BatchResult {
  std::string code;
  std::vector<KLine> bars;
  bool success = false;
};

// 并发批量拉取 K 线。codes 分 N 份，每 worker fiber 独立 Connection 处理分片。
std::vector<BatchResult> BatchFetchKline(const std::vector<std::string>& codes,
                                         int concurrency, Period period,
                                         uint16_t start = 0, uint16_t count = 800);

}  // namespace tdx::batch
