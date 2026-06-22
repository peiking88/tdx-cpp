// BlockReader 实现。对齐 opentdx/utils/block_reader.py:23-72。
#include "tdx/proto/block_reader.hpp"

#include <fstream>
#include <iterator>

#include "tdx/util/byte_order.hpp"
#include "tdx/util/gbk.hpp"

namespace tdx::proto {

std::vector<BlockMember> BlockReader::ReadFile(const std::string& filepath) {
  std::vector<BlockMember> result;
  std::ifstream f(filepath, std::ios::binary);
  if (!f) return result;
  std::vector<uint8_t> data((std::istreambuf_iterator<char>(f)),
                            std::istreambuf_iterator<char>());
  if (data.size() < 384 + 2) return result;

  // 384B 文件头跳过，<H> num（板块数）
  uint16_t num = util::rd_u16(data.data() + 384);
  std::size_t pos = 384 + 2;

  for (uint16_t i = 0; i < num; ++i) {
    if (pos + 2800 > data.size()) break;  // 每块固定 2800B
    BlockMember m;
    // 9B 名(GBK) + <HH>(stock_count, block_type) + stock_count×7B code(UTF-8)
    m.name = util::trim_null(util::gbk_to_utf8(reinterpret_cast<const char*>(data.data() + pos), 9));
    m.stock_count = util::rd_u16(data.data() + pos + 9);
    m.block_type = util::rd_u16(data.data() + pos + 11);
    std::size_t cp = pos + 13;
    for (uint16_t j = 0; j < m.stock_count; ++j) {
      if (cp + 7 > data.size()) break;
      // 成员代码是 UTF-8（block_reader.py 注释明确）
      std::string code(reinterpret_cast<const char*>(data.data() + cp), 7);
      m.codes.push_back(util::trim_null(code));
      cp += 7;
    }
    result.push_back(std::move(m));
    pos += 2800;  // 跳到下一块边界（无论 stock_count 多少）
  }
  return result;
}

std::string BlockReader::DefaultPath(const std::string& tdx_home, BlockFileType type) {
  const char* fname = BlockFileName(type);
  // 通达信板块文件常见位置：T0002/hq_cache/ 或 vipdoc/
  std::string p1 = tdx_home + "/T0002/hq_cache/" + fname;
  std::ifstream f(p1);
  if (f) return p1;
  return tdx_home + "/vipdoc/" + fname;
}

}  // namespace tdx::proto
