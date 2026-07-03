// 共享内存段：mmap + flock 单写者 + magic/version/generation 自描述头。
// 设计文档 §4 SegmentHeader / §5.0.2 单写者 / §D2 /dev/shm tmpfs。
// path 为文件完整路径（建议 /dev/shm/xxx，tmpfs 零磁盘 IO）。
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

#include "tdx/shm/snapshot.hpp"

namespace tdx::shm {

inline constexpr char kMagic[8] = {'T', 'D', 'X', 'S', 'H', 'M', '\0', '\0'};
inline constexpr uint32_t kVersion = 1;

struct SegmentHeader {
  char magic[8];
  uint32_t version;
  uint32_t header_size;
  std::atomic<uint64_t> generation;               // 写者每次 Create +1（读者判重建）
  uint64_t capacity_bytes;
  std::atomic<uint64_t> writer_heartbeat_epoch;   // 写者每轮 fetch 更新
  uint64_t writer_pid;
  uint64_t create_epoch;
  uint32_t snap_slots;
  uint32_t snap_slot_size;
  uint64_t snap_off;
  // ring 区预留（MVP = 0，6.2/6.3 填实）
  uint64_t txn_off, txn_capacity, txn_slot_size;
  uint64_t ord_off, ord_capacity, ord_slot_size;
  uint64_t kmin_off, kmin_capacity, kmin_slot_size;
  std::atomic<uint64_t> last_ingested_epoch;       // IngestWorker 每次 drain 后更新（读者判落库进度）
  char reserved[8];
};

// 布局参数（MVP：仅快照表，16384 槽——设计 §10 Q1 / P2-3）
struct Layout {
  uint32_t snap_slots = 16384;
};

class Segment {
 public:
  // 写者：创建/重建段。flock(LOCK_EX|LOCK_NB) 强制单写者（拿不到返回 nullptr）。
  static std::unique_ptr<Segment> Create(const std::string& path, const Layout& lay);
  // 读者：只读挂载（校验 magic/version/边界）。
  static std::unique_ptr<Segment> OpenReadOnly(const std::string& path);

  ~Segment();
  Segment(const Segment&) = delete;
  Segment& operator=(const Segment&) = delete;

  SegmentHeader& Header() { return *header_; }
  const SegmentHeader& Header() const { return *header_; }
  SnapshotTable& Snapshot() { return snapshot_; }
  const SnapshotTable& Snapshot() const { return snapshot_; }

 private:
  Segment() = default;
  uint8_t* base_ = nullptr;
  size_t size_ = 0;
  int fd_ = -1;
  bool is_writer_ = false;
  SegmentHeader* header_ = nullptr;
  SnapshotTable snapshot_;  // 非 owning 视图，Bind 到 base_+snap_off
};

}  // namespace tdx::shm
