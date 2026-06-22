// 本地板块文件读取器。逐字对齐 opentdx/utils/block_reader.py。
// block.dat/block_zs.dat/block_fg.dat/block_gn.dat：384B 头 + <H>num + 每块固定 2800B。
// 每块：9B 名(GBK) + <HH>(stock_count, block_type) + stock_count×7B code(UTF-8)。
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "tdx/consts.hpp"

namespace tdx::proto {

struct BlockMember {
  std::string name;                   // 板块名 UTF8
  uint16_t stock_count = 0;
  uint16_t block_type = 0;
  std::vector<std::string> codes;     // 成员代码（UTF-8，非 GBK）
};

class BlockReader {
 public:
  // 直接读板块文件
  static std::vector<BlockMember> ReadFile(const std::string& filepath);

  // 从 tdx_home 推断板块文件路径（尝试 T0002/hq_cache 与 vipdoc 两处）
  static std::string DefaultPath(const std::string& tdx_home, BlockFileType type);
};

}  // namespace tdx::proto
