// 信号共享内存段：盘中 czsc 信号实时读写。
// 布局：Header(64B) + Ring Buffer（N × SignalSlot）。
// 只存触发态信号（value != 0），ring buffer 覆盖式。
//
// 写者：tdx czsc（增量计算后推入）
// 读者：czsc-signals（扫描 ring 取最新）
#pragma once

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <string>

namespace tdx::shm {

constexpr const char* kSignalShmDefaultPath = "/dev/shm/tdx_signals.shm";
constexpr uint32_t kSignalShmMagic = 0x5349474E;  // 'SIGN'
constexpr uint32_t kSignalShmVersion = 1;
constexpr uint32_t kSignalSlotSize = 64;          // 每槽 64B
constexpr uint32_t kSignalDefaultSlots = 65536;   // 默认 64K 槽（4MB）

// 信号槽（64B，alignas(64) 缓存行对齐）：只存触发态。
struct alignas(64) SignalSlot {
  char code[8];        // "sh600000\0"
  uint8_t freq;        // Freq 枚举：D=1 F30=2 F5=3 week=4
  uint8_t pad0[7];
  uint64_t name_hash;  // XXH64(signal_name)，dashboard 本地还原
  double value;        // 信号值（0=未触发，不应写入）
  int64_t signal_ts;   // 信号触发时间 epoch 秒
  int64_t bar_ts;      // 对应 bar 时间 epoch 秒
  uint64_t flags;      // bit0=触发 bit1=有效
  uint64_t pad1;       // 填充到 64B
};
static_assert(sizeof(SignalSlot) == 64, "SignalSlot 必须 64B");

// shm 头（64B，独立缓存行）。
struct alignas(64) SignalShmHeader {
  uint32_t magic;       // kSignalShmMagic
  uint32_t version;     // kSignalShmVersion
  uint32_t slot_count;  // ring buffer 槽数
  uint32_t slot_size;   // kSignalSlotSize
  std::atomic<uint64_t> head;  // 原子：下一个写入位置（slot 索引）
  uint64_t pad[5];             // 填充到 64B
};
static_assert(sizeof(SignalShmHeader) == 64, "SignalShmHeader 必须 64B");

// 频率标签 → uint8_t 编码。
inline uint8_t FreqToCode(const std::string& freq_tag) {
  if (freq_tag == "D") return 1;
  if (freq_tag == "F30") return 2;
  if (freq_tag == "F5") return 3;
  if (freq_tag == "week") return 4;
  return 0;
}
inline std::string CodeToFreqTag(uint8_t code) {
  switch (code) {
    case 1: return "D";
    case 2: return "F30";
    case 3: return "F5";
    case 4: return "week";
    default: return "D";
  }
}

// 信号 shm 写入器：创建/打开 + 原子推入触发态信号。
class SignalShmWriter {
 public:
  SignalShmWriter() = default;
  ~SignalShmWriter() { Close(); }

  bool Open(const std::string& path, uint32_t slot_count = kSignalDefaultSlots) {
    path_ = path;
    fd_ = ::open(path.c_str(), O_CREAT | O_RDWR, 0666);
    if (fd_ < 0) return false;
    total_ = static_cast<size_t>(slot_count) * sizeof(SignalSlot) + sizeof(SignalShmHeader);
    if (::ftruncate(fd_, static_cast<off_t>(total_)) < 0) { Close(); return false; }
    base_ = ::mmap(nullptr, total_, PROT_READ | PROT_WRITE, MAP_SHARED, fd_, 0);
    if (base_ == MAP_FAILED) { base_ = nullptr; Close(); return false; }
    header_ = static_cast<SignalShmHeader*>(base_);
    if (header_->magic != kSignalShmMagic || header_->version != kSignalShmVersion) {
      std::memset(base_, 0, total_);
      header_->magic = kSignalShmMagic;
      header_->version = kSignalShmVersion;
      header_->slot_count = slot_count;
      header_->slot_size = kSignalSlotSize;
      header_->head = 0;
    }
    slots_ = reinterpret_cast<SignalSlot*>(static_cast<char*>(base_) + sizeof(SignalShmHeader));
    return true;
  }

  void Close() {
    if (base_ && base_ != MAP_FAILED) { ::munmap(base_, total_); base_ = nullptr; }
    if (fd_ >= 0) { ::close(fd_); fd_ = -1; }
  }

  // 推入触发态信号（value == 0 不写）。
  bool Push(const char* code, uint8_t freq, uint64_t name_hash,
            double value, int64_t signal_ts, int64_t bar_ts) {
    if (!base_ || value == 0.0) return false;
    uint64_t pos = header_->head.fetch_add(1, std::memory_order_relaxed);
    uint32_t idx = static_cast<uint32_t>(pos % header_->slot_count);
    SignalSlot* s = slots_ + idx;
    std::memset(s, 0, sizeof(*s));
    std::strncpy(s->code, code, sizeof(s->code) - 1);
    s->freq = freq;
    s->name_hash = name_hash;
    s->value = value;
    s->signal_ts = signal_ts;
    s->bar_ts = bar_ts;
    s->flags = 0x3;
    return true;
  }

  uint32_t slot_count() const { return header_ ? header_->slot_count : 0; }

 private:
  std::string path_;
  int fd_ = -1;
  void* base_ = nullptr;
  size_t total_ = 0;
  SignalShmHeader* header_ = nullptr;
  SignalSlot* slots_ = nullptr;
};

}  // namespace tdx::shm
