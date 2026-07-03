// 共享内存段 mmap 生命周期。设计文档 §4 SegmentHeader / §5.0.2 单写者 flock。
#include "tdx/shm/segment.hpp"

#include <fcntl.h>
#include <sys/file.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <cstring>

namespace tdx::shm {

static size_t AlignUp(size_t v, size_t a) { return (v + a - 1) & ~(a - 1); }

std::unique_ptr<Segment> Segment::Create(const std::string& path, const Layout& lay) {
  int fd = ::open(path.c_str(), O_CREAT | O_RDWR, 0600);
  if (fd < 0) return nullptr;
  // flock 排他：强制单写者（设计 §5.0.2）。拿不到 → 已有写者 → 退出。
  if (::flock(fd, LOCK_EX | LOCK_NB) != 0) {
    ::close(fd);
    return nullptr;
  }

  const size_t snap_off = AlignUp(sizeof(SegmentHeader), 64);
  const size_t cap = snap_off + size_t(lay.snap_slots) * sizeof(SnapSlot);
  if (::ftruncate(fd, off_t(cap)) != 0) {
    ::close(fd);
    return nullptr;
  }

  void* base = ::mmap(nullptr, cap, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  if (base == MAP_FAILED) {
    ::close(fd);
    return nullptr;
  }

  auto seg = std::unique_ptr<Segment>(new Segment());
  seg->base_ = static_cast<uint8_t*>(base);
  seg->size_ = cap;
  seg->fd_ = fd;
  seg->is_writer_ = true;
  seg->header_ = static_cast<SegmentHeader*>(base);

  // 路径复用（上次写者崩溃留下段）：保留旧 generation 自增；否则从 1 起。
  uint64_t prev_gen = 0;
  if (std::memcmp(seg->header_->magic, kMagic, 8) == 0) {
    prev_gen = seg->header_->generation.load(std::memory_order_relaxed);
  }

  SegmentHeader& h = *seg->header_;
  std::memcpy(h.magic, kMagic, 8);
  h.version = kVersion;
  h.header_size = sizeof(SegmentHeader);
  h.generation.store(prev_gen + 1, std::memory_order_release);
  h.capacity_bytes = cap;
  h.writer_heartbeat_epoch.store(0, std::memory_order_relaxed);
  h.writer_pid = static_cast<uint64_t>(::getpid());
  h.create_epoch = static_cast<uint64_t>(::time(nullptr));
  h.snap_slots = lay.snap_slots;
  h.snap_slot_size = sizeof(SnapSlot);
  h.snap_off = snap_off;
  h.txn_off = h.txn_capacity = h.txn_slot_size = 0;
  h.ord_off = h.ord_capacity = h.ord_slot_size = 0;
  h.kmin_off = h.kmin_capacity = h.kmin_slot_size = 0;
  std::memset(h.reserved, 0, sizeof(h.reserved));

  // 快照区清零（seq=0 偶=稳定空槽，code 全 0）
  SnapSlot* slots = reinterpret_cast<SnapSlot*>(seg->base_ + snap_off);
  std::memset(slots, 0, size_t(lay.snap_slots) * sizeof(SnapSlot));
  seg->snapshot_.Bind(slots, lay.snap_slots);
  return seg;
}

std::unique_ptr<Segment> Segment::OpenReadOnly(const std::string& path) {
  int fd = ::open(path.c_str(), O_RDONLY, 0);
  if (fd < 0) return nullptr;
  struct stat st{};
  if (::fstat(fd, &st) != 0 || st.st_size < off_t(sizeof(SegmentHeader))) {
    ::close(fd);
    return nullptr;
  }
  size_t cap = st.st_size;
  void* base = ::mmap(nullptr, cap, PROT_READ, MAP_SHARED, fd, 0);
  if (base == MAP_FAILED) {
    ::close(fd);
    return nullptr;
  }

  const SegmentHeader* h = static_cast<const SegmentHeader*>(base);
  if (std::memcmp(h->magic, kMagic, 8) != 0 || h->version != kVersion ||
      h->snap_off + uint64_t(h->snap_slots) * h->snap_slot_size > cap) {
    ::munmap(base, cap);
    ::close(fd);
    return nullptr;
  }

  auto seg = std::unique_ptr<Segment>(new Segment());
  seg->base_ = static_cast<uint8_t*>(base);
  seg->size_ = cap;
  seg->fd_ = fd;
  seg->is_writer_ = false;
  seg->header_ = static_cast<SegmentHeader*>(base);  // PROT_READ，仅 load（只读访问）
  SnapSlot* slots = reinterpret_cast<SnapSlot*>(seg->base_ + h->snap_off);
  seg->snapshot_.Bind(slots, h->snap_slots);
  return seg;
}

Segment::~Segment() {
  if (base_ && base_ != MAP_FAILED) ::munmap(base_, size_);
  if (fd_ >= 0) {
    if (is_writer_) ::flock(fd_, LOCK_UN);  // 释放写者排他锁
    ::close(fd_);
  }
}

}  // namespace tdx::shm
