// 共享内存定长 POD 载荷：与 tdx::types 互转。
// intraday 路径下 Quote.name 恒空、code 6 字节（parsers_quotes.cpp 只填 code），
// 故 POD 化丢弃 name、code 定长 8 字节。设计文档 §2.1 / §4 SnapSlot。
#pragma once

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string_view>
#include <type_traits>

#include "tdx/types.hpp"

namespace tdx::shm {

// Quote 的定长 POD 视图（不含 name，不含 code——code 放 SnapSlot 外层）。
// 全字段 8 字节对齐，无 padding：datetime(8) + 7×double(56) + 20×double(160) = 224B。
struct QuotePOD {
  int64_t datetime = 0;
  double price = 0;
  double pre_close = 0;
  double open = 0;
  double high = 0;
  double low = 0;
  double volume = 0;
  double amount = 0;
  double bid[5]{};
  double ask[5]{};
  double bid_vol[5]{};
  double ask_vol[5]{};
};
static_assert(sizeof(QuotePOD) == 224, "QuotePOD 定长 224B");
static_assert(std::is_trivially_copyable<QuotePOD>::value, "QuotePOD 须可平凡拷贝（mmap/seqlock memcpy）");

// code 拷贝到定长 8 字节缓冲（截断到 7 字符 + null，其余清零）。
inline void CopyCode(char dst[8], std::string_view code) {
  size_t n = std::min<size_t>(code.size(), 7);
  std::memcpy(dst, code.data(), n);
  dst[n] = '\0';
  for (size_t i = n + 1; i < 8; ++i) dst[i] = '\0';
}

inline QuotePOD to_pod(const tdx::Quote& q) {
  QuotePOD p;
  p.datetime = q.datetime;
  p.price = q.price; p.pre_close = q.pre_close; p.open = q.open;
  p.high = q.high; p.low = q.low; p.volume = q.volume; p.amount = q.amount;
  for (int k = 0; k < 5; ++k) {
    p.bid[k] = q.bid[k]; p.ask[k] = q.ask[k];
    p.bid_vol[k] = q.bid_vol[k]; p.ask_vol[k] = q.ask_vol[k];
  }
  return p;
}

inline tdx::Quote from_pod(const QuotePOD& p) {
  tdx::Quote q;  // 构造时 bid/ask 填 NaN 哨兵（types.hpp 约定）
  q.datetime = p.datetime;
  q.price = p.price; q.pre_close = p.pre_close; q.open = p.open;
  q.high = p.high; q.low = p.low; q.volume = p.volume; q.amount = p.amount;
  for (int k = 0; k < 5; ++k) {
    q.bid[k] = p.bid[k]; q.ask[k] = p.ask[k];
    q.bid_vol[k] = p.bid_vol[k]; q.ask_vol[k] = p.ask_vol[k];
  }
  return q;
}

}  // namespace tdx::shm
