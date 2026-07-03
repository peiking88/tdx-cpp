// Phase 6 共享内存单元测试。
// 设计文档 §9：seqlock 单写多读无撕裂、flock 拒第二写者、POD 全字段转换、fork 父写子读。
// 测试代码豁免 fiber 纪律（评审 #3）：用 std::thread / fork，跑在 gtest 线程。
#include <fcntl.h>
#include <pthread.h>
#include <sys/wait.h>
#include <unistd.h>

#include <atomic>
#include <cmath>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "tdx/shm/payload.hpp"
#include "tdx/shm/segment.hpp"

namespace {
std::string TestPath(const char* tag) {
  return "/dev/shm/tdx_test_" + std::string(tag) + "_" + std::to_string(::getpid());
}
}  // namespace

// ---- POD 全字段 round-trip（含 NaN 哨兵）----
TEST(ShmPayload, RoundTrip) {
  tdx::Quote q;
  q.code = "600000";
  q.datetime = 1234567890;
  q.price = 12.34;
  q.pre_close = 11.11;
  q.open = 11.5;
  q.high = 12.5;
  q.low = 11.0;
  q.volume = 1000000;
  q.amount = 12345678.9;
  for (int k = 0; k < 5; ++k) {
    q.bid[k] = 10 + k;
    q.ask[k] = 11 + k;
    q.bid_vol[k] = 100 + k;
    q.ask_vol[k] = 200 + k;
  }
  tdx::shm::QuotePOD pod = tdx::shm::to_pod(q);
  tdx::Quote q2 = tdx::shm::from_pod(pod);
  EXPECT_EQ(q2.datetime, q.datetime);
  EXPECT_DOUBLE_EQ(q2.price, q.price);
  EXPECT_DOUBLE_EQ(q2.pre_close, q.pre_close);
  EXPECT_DOUBLE_EQ(q2.amount, q.amount);
  for (int k = 0; k < 5; ++k) {
    EXPECT_DOUBLE_EQ(q2.bid[k], q.bid[k]);
    EXPECT_DOUBLE_EQ(q2.ask_vol[k], q.ask_vol[k]);
  }
  // NaN 哨兵 round-trip（未设档位保持 NaN）
  tdx::Quote q3;  // 构造填 NaN
  EXPECT_TRUE(std::isnan(q3.bid[0]));
  tdx::shm::QuotePOD pod3 = tdx::shm::to_pod(q3);
  tdx::Quote q4 = tdx::shm::from_pod(pod3);
  EXPECT_TRUE(std::isnan(q4.bid[0]));
  EXPECT_TRUE(std::isnan(q4.ask[4]));
}

// ---- Segment Create + Put + Get（单进程基础）----
TEST(ShmSegment, CreatePutGet) {
  std::string path = TestPath("putget");
  auto seg = tdx::shm::Segment::Create(path, {});
  ASSERT_TRUE(seg);
  EXPECT_EQ(seg->Header().snap_slots, 16384u);
  EXPECT_GT(seg->Header().generation, 0u);

  tdx::shm::QuotePOD q;
  q.price = 99.5;
  q.volume = 8888;
  seg->Snapshot().Put("600000", q);

  tdx::shm::QuotePOD out;
  ASSERT_TRUE(seg->Snapshot().Get("600000", out));
  EXPECT_DOUBLE_EQ(out.price, 99.5);
  EXPECT_DOUBLE_EQ(out.volume, 8888);
  EXPECT_FALSE(seg->Snapshot().Get("999999", out));  // 未命中

  seg.reset();
  ::unlink(path.c_str());
}

// ---- flock 拒第二写者（单写者强制，设计 §5.0.2）----
TEST(ShmSegment, FlockRejectSecondWriter) {
  std::string path = TestPath("flock");
  auto seg1 = tdx::shm::Segment::Create(path, {});
  ASSERT_TRUE(seg1);
  auto seg2 = tdx::shm::Segment::Create(path, {});  // 同路径第二写者
  EXPECT_FALSE(seg2);                                // flock LOCK_EX|LOCK_NB 拒绝
  seg1.reset();
  ::unlink(path.c_str());
}

// ---- OpenReadOnly 拒坏 magic----
TEST(ShmSegment, OpenReadOnlyRejectsBadMagic) {
  std::string path = TestPath("badmagic");
  // 写一个非段文件
  int fd = ::open(path.c_str(), O_CREAT | O_RDWR, 0600);
  ASSERT_GE(fd, 0);
  char buf[256] = "not a shm segment";
  EXPECT_EQ(::write(fd, buf, sizeof(buf)), (ssize_t)sizeof(buf));
  ::close(fd);
  EXPECT_FALSE(tdx::shm::Segment::OpenReadOnly(path));
  ::unlink(path.c_str());
}

// ---- fork 父写子读（跨进程正确性 + P0-1 双进程原子性实测）----
TEST(ShmSegment, ForkWriteRead) {
  std::string path = TestPath("fork");
  auto seg = tdx::shm::Segment::Create(path, {});
  ASSERT_TRUE(seg);
  tdx::shm::QuotePOD q;
  q.price = 42.0;
  q.volume = 12345;
  seg->Snapshot().Put("000001", q);

  pid_t pid = ::fork();
  ASSERT_GE(pid, 0);
  if (pid == 0) {  // 子进程：重新只读挂载
    auto rseg = tdx::shm::Segment::OpenReadOnly(path);
    if (!rseg) ::_exit(2);
    tdx::shm::QuotePOD out;
    if (!rseg->Snapshot().Get("000001", out)) ::_exit(3);
    if (out.price != 42.0) ::_exit(4);
    if (out.volume != 12345) ::_exit(5);
    ::_exit(0);
  }
  int status = 0;
  ::waitpid(pid, &status, 0);
  EXPECT_TRUE(WIFEXITED(status)) << "子进程异常退出";
  EXPECT_EQ(WEXITSTATUS(status), 0) << "子进程退出码非 0";

  seg.reset();
  ::unlink(path.c_str());
}

// ---- seqlock 单写多读无撕裂（多线程压测）----
// 写者写 price=i, volume=i（两字段关联）；读者校验每次读到 price==volume。
// 撕裂会被 seq 检测重试（Get 返回 false 跳过），故 mism 恒为 0。
TEST(ShmSnapshot, SeqlockNoTear) {
  std::string path = TestPath("seqlock");
  auto seg = tdx::shm::Segment::Create(path, {});
  ASSERT_TRUE(seg);

  std::atomic<bool> stop{false};
  std::atomic<int64_t> mism{0}, ok{0};

  std::thread writer([&] {
    tdx::shm::QuotePOD q;
    for (int i = 0; !stop; ++i) {
      q.price = static_cast<double>(i);
      q.volume = static_cast<double>(i);  // price 与 volume 关联
      seg->Snapshot().Put("600000", q);
    }
  });

  std::vector<std::thread> readers;
  for (int t = 0; t < 4; ++t) {
    readers.emplace_back([&] {
      tdx::shm::QuotePOD out;
      for (int n = 0; n < 200000 && !stop; ++n) {
        if (seg->Snapshot().Get("600000", out)) {
          if (out.price != out.volume) mism++;
          else ok++;
        }
      }
    });
  }

  for (auto& r : readers) r.join();
  stop = true;
  writer.join();

  EXPECT_GT(ok.load(), 0) << "读者应至少成功读到一次";
  EXPECT_EQ(mism.load(), 0) << "seqlock 撕裂：price 与 volume 不一致";

  seg.reset();
  ::unlink(path.c_str());
}

// ---- 哈希冲突：不同 code 不互相误命中（线性探测 + code 比对）----
TEST(ShmSnapshot, HashCollisionNotFalseHit) {
  std::string path = TestPath("collision");
  auto seg = tdx::shm::Segment::Create(path, {});
  ASSERT_TRUE(seg);
  tdx::shm::QuotePOD q;
  q.price = 1.0;
  seg->Snapshot().Put("600000", q);
  q.price = 2.0;
  seg->Snapshot().Put("600001", q);  // 相邻 code，可能哈希到同槽附近

  tdx::shm::QuotePOD out;
  ASSERT_TRUE(seg->Snapshot().Get("600000", out));
  EXPECT_DOUBLE_EQ(out.price, 1.0);
  ASSERT_TRUE(seg->Snapshot().Get("600001", out));
  EXPECT_DOUBLE_EQ(out.price, 2.0);
  EXPECT_FALSE(seg->Snapshot().Get("600099", out));

  seg.reset();
  ::unlink(path.c_str());
}
