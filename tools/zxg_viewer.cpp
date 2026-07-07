// 自选股实时行情终端查看器：从 zxg.blk 读自选列表 + mmap 共享内存读行情，3秒刷新。
// ponytail: 独立二进制，不跑 gtest（终端交互不适合 unit test）。
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>
#include <thread>
#include "tdx/shm/segment.hpp"
#include "tdx/shm/payload.hpp"

namespace fs = std::filesystem;

// 读通达信自选股 .blk 文件：纯文本，每行 7 位代码，\r\n 分隔。
static std::vector<std::string> LoadBlk(const char* path) {
  std::vector<std::string> codes;
  std::ifstream f(path);
  if (!f.is_open()) { std::fprintf(stderr, "无法打开自选股文件: %s\n", path); return codes; }
  std::string line;
  while (std::getline(f, line)) {
    // 去掉 \r
    while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.pop_back();
    if (line.size() == 7 && (line[0] == '1' || line[0] == '0' || line[0] == '3'))
      line = line.substr(1);  // 通达信市场标记位，shm 存的是 6 位代码
    if (line.size() >= 6) codes.push_back(line);
  }
  return codes;
}

static void PrintUsage(const char* prog) {
  std::fprintf(stderr, "用法: %s [--shm /dev/shm/tdx_quotes.shm] [--blk /path/to/zxg.blk]\n", prog);
  std::fprintf(stderr, "  默认 shm=/dev/shm/tdx_quotes.shm\n");
  std::fprintf(stderr, "  默认 blk=~/.local/share/tdxcfv/drive_c/tc/T0002/blocknew/zxg.blk\n");
}

int main(int argc, char* argv[]) {
  std::string shm_path = "/dev/shm/tdx_quotes.shm";
  std::string blk_path = std::string(std::getenv("HOME")) +
      "/.local/share/tdxcfv/drive_c/tc/T0002/blocknew/zxg.blk";

  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--shm") == 0 && i + 1 < argc) shm_path = argv[++i];
    else if (std::strcmp(argv[i], "--blk") == 0 && i + 1 < argc) blk_path = argv[++i];
    else if (std::strcmp(argv[i], "--help") == 0 || std::strcmp(argv[i], "-h") == 0) { PrintUsage(argv[0]); return 0; }
  }

  auto codes = LoadBlk(blk_path.c_str());
  if (codes.empty()) { std::fprintf(stderr, "自选股为空\n"); return 1; }

  auto seg = tdx::shm::Segment::OpenReadOnly(shm_path);
  if (!seg) { std::fprintf(stderr, "无法打开共享内存: %s（请先启动 fetch-quotes --mmap_path %s）\n",
                           shm_path.c_str(), shm_path.c_str()); return 1; }

  const auto& snap = seg->Snapshot();

  while (true) {
    // ANSI 清屏+光标归位
    std::printf("\033[2J\033[H");
    std::printf("=== 自选股实时行情（每3秒刷新）===  共 %zu 只 | shm: %s\n\n", codes.size(), shm_path.c_str());
    std::printf(" %-8s %8s %8s %8s %8s %10s %12s %7s\n",
                "代码", "现价", "昨收", "开盘", "最高", "成交量", "成交额", "涨跌%%");
    std::printf(" %.8s %.8s %.8s %.8s %.8s %.10s %.12s %.7s\n",
                "--------", "--------", "--------", "--------", "--------", "----------", "------------", "-------");

    for (const auto& c : codes) {
      tdx::shm::QuotePOD q;
      if (!snap.Get(c, q)) {
        std::printf(" %-8s (无数据)\n", c.c_str());
        continue;
      }

      double chg_pct = 0;
      if (q.pre_close > 0) chg_pct = (q.price - q.pre_close) / q.pre_close * 100.0;

      // 涨跌色
      const char* color = "\033[0m";
      if (chg_pct > 0.01) color = "\033[31m";      // 红涨
      else if (chg_pct < -0.01) color = "\033[32m"; // 绿跌
      const char* reset = "\033[0m";

      std::printf(" %s%-8s%s %8.2f %8.2f %8.2f %8.2f %10.0f %12.0f %6.1f%%\n",
                  color, c.c_str(), reset,
                  q.price, q.pre_close, q.open, q.high,
                  q.volume, q.amount, chg_pct);
    }

    std::printf("\n按 Ctrl+C 退出\n");
    std::fflush(stdout);
    std::this_thread::sleep_for(std::chrono::seconds(3));
  }
}
