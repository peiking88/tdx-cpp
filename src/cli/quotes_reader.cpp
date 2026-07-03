// tdx_quotes_reader：只读挂载盘中实时行情共享内存，轮询打印某股票最新报价。
// 用法：tdx_quotes_reader <mmap_path> <code> [interval_ms]
// 设计文档 §6.3 reader 示例。与 fetch-quotes 写者通过同一 mmap 文件互通。
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>

#include "tdx/shm/segment.hpp"

std::atomic<bool> g_stop{false};
void OnSignal(int) { g_stop = true; }

int main(int argc, char** argv) {
  if (argc < 3) {
    std::cerr << "用法: " << argv[0] << " <mmap_path> <code> [interval_ms=500]\n";
    return 1;
  }
  std::string path = argv[1];
  std::string code = argv[2];
  int interval_ms = (argc >= 4) ? std::atoi(argv[3]) : 500;

  auto seg = tdx::shm::Segment::OpenReadOnly(path);
  if (!seg) {
    std::cerr << "挂载共享内存失败: " << path
              << "（采集进程是否已启动？段是否已创建？）\n";
    return 1;
  }
  std::cerr << "已挂载: " << path
            << " generation=" << seg->Header().generation.load()
            << " snap_slots=" << seg->Snapshot().slots() << "\n";

  std::signal(SIGINT, OnSignal);
  std::signal(SIGTERM, OnSignal);

  while (!g_stop) {
    tdx::shm::QuotePOD q;
    if (seg->Snapshot().Get(code, q)) {
      std::printf("[%s] 现价:%.2f 开:%.2f 高:%.2f 低:%.2f "
                  "量:%.0f 额:%.2f 买1:%.2f×%.0f 卖1:%.2f×%.0f\n",
                  code.c_str(), q.price, q.open, q.high, q.low,
                  q.volume, q.amount, q.bid[0], q.bid_vol[0], q.ask[0], q.ask_vol[0]);
      std::fflush(stdout);
    } else {
      std::printf("[%s] 未命中（采集进程尚未写入该代码）\n", code.c_str());
      std::fflush(stdout);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(interval_ms));
  }
  return 0;
}
