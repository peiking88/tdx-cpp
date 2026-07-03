// 快照表 seqlock 实现。设计文档 §5.1（v0.3：release 配对，无冗余 seq_cst fence）。
#include "tdx/shm/snapshot.hpp"

namespace tdx::shm {

uint32_t SnapshotTable::Hash(std::string_view code) {
  uint32_t h = 2166136261u;  // FNV-1a offset basis
  for (unsigned char c : code) {
    h ^= c;
    h *= 16777619u;
  }
  return h;
}

// 写者（采集主线程，唯一）。upsert 语义：空槽或 code 匹配槽写入。
void SnapshotTable::Put(std::string_view code, const QuotePOD& q) {
  if (code.empty() || code.size() > 7 || count_ == 0) return;
  const uint32_t mask = count_ - 1;  // count_ 须 2 的幂
  uint32_t idx = Hash(code) & mask;
  for (uint32_t probe = 0; probe < count_; ++probe) {
    SnapSlot& s = slots_[(idx + probe) & mask];
    char existing[8];
    std::memcpy(existing, s.code, 8);  // 单写者，无需 seq 保护
    bool empty = (existing[0] == '\0');
    bool match = !empty && code.size() <= 7 &&
                 std::memcmp(existing, code.data(), code.size()) == 0 &&
                 existing[code.size()] == '\0';
    if (!empty && !match) continue;  // 占用且非目标 → 线性探测下一槽

    // seqlock 写：seq 偶→奇(release) → 载荷 → 奇→偶(release)。
    // release store 保证载荷写不被重排到 seq store 之前/之后，读者 acquire 配对。
    uint64_t s0 = s.seq.load(std::memory_order_relaxed);
    s.seq.store(s0 + 1, std::memory_order_release);  // 奇：进入写
    CopyCode(s.code, code);
    s.q = q;
    s.flags = 1;
    s.seq.store(s0 + 2, std::memory_order_release);  // 偶：写完
    return;
  }
  // 表满（16384 槽对 ~8000 股不会发生）— 丢弃
}

// 读者（多进程，无锁）。false = 未命中/写中/撕裂重试耗尽。
bool SnapshotTable::Get(std::string_view code, QuotePOD& out) const {
  if (code.empty() || code.size() > 7 || count_ == 0) return false;
  const uint32_t mask = count_ - 1;
  for (int retry = 0; retry < 8; ++retry) {
    uint32_t idx = Hash(code) & mask;
    for (uint32_t probe = 0; probe < count_; ++probe) {
      const SnapSlot& s = slots_[(idx + probe) & mask];
      uint64_t s1 = s.seq.load(std::memory_order_acquire);
      if (s1 & 1) break;  // 写中 → 本轮重试

      char c[8];
      std::memcpy(c, s.code, 8);
      QuotePOD qcopy;
      std::memcpy(&qcopy, &s.q, sizeof(qcopy));
      (void)s.flags;
      std::atomic_thread_fence(std::memory_order_acquire);  // 阻止数据读后移到 s2 load 之后（ARM64/LTO 下否则撕裂）
      uint64_t s2 = s.seq.load(std::memory_order_relaxed);
      if (s1 != s2) break;  // 写者穿插撕裂 → 本轮重试

      if (c[0] == '\0') return false;  // 空槽：键不存在（线性探测终止）
      if (std::memcmp(c, code.data(), code.size()) == 0 && c[code.size()] == '\0') {
        out = qcopy;
        return true;
      }
      // 哈希冲突：继续探测下一槽
    }
  }
  return false;  // 重试耗尽（持续写中或持续撕裂）
}

}  // namespace tdx::shm
