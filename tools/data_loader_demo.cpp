// czsc_data_viewer：日线拼接查看器（仿 tdx-cpp/tools/mmap_viewer.cpp）。
// 用法: czsc_data_viewer [sz002515] [--shm /dev/shm/tdx_quotes.shm] [-n 8]
// ponytail: 独立二进制，人工 eyeball 用，不跑 gtest。
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>

#include "czsc/io/data_loader.hpp"
#include "czsc/io/signal_store.hpp"
#include "tdx/shm/payload.hpp"
#include "tdx/shm/segment.hpp"

static char* FmtDay(int64_t epoch_sec, char* buf, size_t n) {
  time_t t = static_cast<time_t>(epoch_sec);
  struct tm tmv;
  ::localtime_r(&t, &tmv);  // 本机 CST
  std::snprintf(buf, n, "%04d-%02d-%02d", tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday);
  return buf;
}  // buf 须 ≥ 11 字节；调用方统一传 32 字节缓冲。

int main(int argc, char* argv[]) {
  std::string raw = "sz002515";
  int n = 8;
  czsc::io::LoaderConfig cfg = czsc::io::LoaderConfig::FromEnv();
  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    if ((a == "--shm") && i + 1 < argc) cfg.shm_path = argv[++i];
    else if ((a == "-n") && i + 1 < argc) n = std::atoi(argv[++i]);
    else if (a == "-h" || a == "--help") {
      std::printf("用法: %s [sh/sz/bj/hk+代码] [--shm PATH] [-n N]\n", argv[0]);
      return 0;
    } else if (a[0] != '-') raw = a;
  }

  auto key = czsc::io::NormalizeCode(raw);
  if (!key) {
    std::fprintf(stderr, "代码非法：%s（必须带 sh/sz/bj/hk 前缀，不接受裸码）\n", raw.c_str());
    return 1;
  }
  const std::string& code = key->code;
  const czsc::Market mkt = czsc::PrefixToMarket(key->mkt);

  auto bars = czsc::io::LoadDailyBars(code, cfg, mkt);
  if (bars.empty()) {
    std::fprintf(stderr, "加载失败：%s（TDengine 不可达或 k_%s%s_1d 不存在）\n",
                 key->display.c_str(), key->mkt.c_str(), code.c_str());
    return 1;
  }

  // 判定末尾 bar 是否来自 mmap 实时（shm 可读且含该 code）。
  bool live = false;
  if (auto seg = tdx::shm::Segment::OpenReadOnly(cfg.shm_path)) {
    tdx::shm::QuotePOD q{};
    live = seg->Snapshot().Get(code, q);
  }

  char d0[32], d1[32];
  std::printf("=== %s 日线（%zu 根，%s ~ %s）%s===\n",
              key->display.c_str(), bars.size(), FmtDay(bars.front().dt, d0, sizeof(d0)),
              FmtDay(bars.back().dt, d1, sizeof(d1)), live ? "  [末bar=实时拼接] " : "");
  std::printf(" %-12s %8s %8s %8s %8s %12s %14s\n",
              "日期", "开", "高", "低", "收", "成交量", "成交额");
  int start = static_cast<int>(bars.size()) - n;
  if (start < 0) start = 0;
  for (int i = start; i < static_cast<int>(bars.size()); ++i) {
    const auto& b = bars[i];
    char d[32];
    std::printf(" %-12s %8.2f %8.2f %8.2f %8.2f %12.0f %14.0f\n",
                FmtDay(b.dt, d, sizeof(d)), b.open, b.high, b.low, b.close, b.vol, b.amount);
  }
  return 0;
}
