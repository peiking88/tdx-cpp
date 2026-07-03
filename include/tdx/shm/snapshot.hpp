// 共享内存快照表：定长 SnapSlot + seqlock 单写多读。
// 设计文档 §4 SnapSlot / §5.0 P0 invariant / §5.1 seqlock（v0.3，无冗余 fence）。
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string_view>

#include "tdx/shm/payload.hpp"

namespace tdx::shm {

// seqlock 槽：seq 偶=稳定 / 奇=写中。alignas(64) 保证 seq 起始于 cache line。
// 布局：seq(8) + code(8) + QuotePOD(224) + flags(4) + pad(12) = 256B。
struct alignas(64) SnapSlot {
  std::atomic<uint64_t> seq;   // 偶=稳定，奇=写中
  char code[8];                // "600000"；全 0 = 空槽
  QuotePOD q;
  uint32_t flags;              // bit0 = 有效载荷
  char pad[12];                // 凑 256B
};

// 设计文档 §四「布局不变量」（编译期拦截，P0-1）
static_assert(std::atomic<uint64_t>::is_always_lock_free,
              "shm 原子字段必须 lock-free，否则跨进程静默失效");
static_assert(std::atomic<uint32_t>::is_always_lock_free, "...");
static_assert(alignof(SnapSlot) == 64, "SnapSlot 需 64B 对齐");
static_assert(offsetof(SnapSlot, seq) == 0, "seq 必须起始偏移 0");
static_assert(sizeof(SnapSlot) == 256, "SnapSlot 定长 256B");

// 快照表：单写者 Put / 多读者 Get（无锁）。
// 槽数须为 2 的幂（哈希掩码 & (count-1)）。哈希冲突用线性探测。
class SnapshotTable {
 public:
  SnapshotTable() = default;
  void Bind(SnapSlot* slots, uint32_t count) { slots_ = slots; count_ = count; }

  void Put(std::string_view code, const QuotePOD& q);   // 写者（唯一）
  bool Get(std::string_view code, QuotePOD& out) const;  // 读者（无锁）

  uint32_t slots() const { return count_; }
  SnapSlot* slot(uint32_t i) { return &slots_[i]; }     // 测试/扫描用
  const SnapSlot* slot(uint32_t i) const { return &slots_[i]; }

 private:
  static uint32_t Hash(std::string_view code);
  SnapSlot* slots_ = nullptr;
  uint32_t count_ = 0;
};

}  // namespace tdx::shm
