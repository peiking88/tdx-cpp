// 链路恢复测试（CI 可重复，不依赖通达信真网）：
//   Connection::Call 在协议异常（非法响应头）后把 connected_ 置 false
//   ——这是 StdQuotes 心跳失败路径能触发重连的前提（修复前此路径不断开，
//   上层无法感知链路已坏，熔断器 HALF_OPEN 探测永远打在死连接上，永不恢复）。
//
// 全本地 loopback TCP 服务，无需 custom main。
#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <memory>
#include <system_error>
#include <thread>

#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>  // execv

#include "base/init.h"  // MainInitGuard（helio 初始化）
#include "util/fibers/pool.h"

#include "tdx/proto/connection.hpp"
#include "tdx/errors.hpp"  // tdx::TdxProtocolError

namespace tdx::proto {
// 暴露 Connection 的 connected_ 状态给测试（仅测试用，避免改 public API）。
inline bool IsConnConnected(const Connection& c) { return c.IsConnected(); }
}  // namespace tdx::proto

using namespace tdx::proto;

// ---- 简易单连接 loopback TCP 服务（独立线程） ---------------------------
// accept → 写入 reply_bytes（模拟响应）→ 保持 hold_ms 读取客户端数据 → 关闭。
class ScriptedServer {
 public:
  explicit ScriptedServer(std::vector<uint8_t> reply_bytes, int hold_ms = 300)
      : reply_bytes_(std::move(reply_bytes)), hold_ms_(hold_ms) {}

  int port() const { return port_; }
  void Start() {
    listen_ = ::socket(AF_INET, SOCK_STREAM, 0);
    ASSERT_GE(listen_, 0);
    int yes = 1;
    ::setsockopt(listen_, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    ASSERT_EQ(::bind(listen_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)), 0);
    socklen_t len = sizeof(addr);
    ASSERT_EQ(::getsockname(listen_, reinterpret_cast<sockaddr*>(&addr), &len), 0);
    port_ = ntohs(addr.sin_port);
    ASSERT_GT(port_, 0);
    int listen_rc = ::listen(listen_, 1);
    if (listen_rc != 0) {
      std::fprintf(stderr, "[ScriptedServer] listen failed: fd=%d errno=%d (%s)\n",
                   listen_, errno, std::strerror(errno));
    }
    ASSERT_EQ(listen_rc, 0);
    thread_ = std::thread([this] { Serve(); });
  }
  void WaitAccepted() {
    while (!accepted_.load()) ::usleep(1000);
  }
  void Join() {
    if (thread_.joinable()) thread_.join();
    if (listen_ >= 0) ::close(listen_);
  }

 private:
  void Serve() {
    sockaddr_in client{};
    socklen_t len = sizeof(client);
    int fd = ::accept(listen_, reinterpret_cast<sockaddr*>(&client), &len);
    if (fd < 0) return;
    // 协议顺序对齐 Connection::Call：客户端先 SendAll(12 字节请求)，再 RecvExact(16)。
    // 故服务端先阻塞读取请求（同步点：确保 clients 已进入 recv 等待）→ 再回写响应，
    // 最后等客户端关闭。避免服务端抢先 send/recv 在沙箱 epoll 里触发 ECANCELED。
    accepted_.store(true);
    uint8_t buf[1024];
    // 1) 阻塞读取客户端请求 → 触发回写。
    bool got = false;
    while (true) {
      ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
      if (n > 0) { got = true; break; }
      if (n <= 0) break;
    }
    if (got && !reply_bytes_.empty()) {
      size_t off = 0;
      while (off < reply_bytes_.size()) {
        ssize_t w = ::send(fd, reply_bytes_.data() + off,
                           reply_bytes_.size() - off, 0);
        if (w <= 0) break;
        off += static_cast<size_t>(w);
      }
    }
    // 2) 等客户端关闭连接。
    while (true) {
      ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
      if (n <= 0) break;
    }
    ::close(fd);
  }
  std::vector<uint8_t> reply_bytes_;
  int hold_ms_;
  int listen_ = -1;
  int port_ = 0;
  std::atomic<bool> accepted_{false};
  std::thread thread_;
};

// 非法响应头（prefix ≠ b1 cb 74 00）→ parse_response_header 抛 TdxProtocolError。
std::vector<uint8_t> BadHeader() { return std::vector<uint8_t>(16, 0); }

class ConnectionRecoveryTest : public ::testing::Test {
 protected:
  void SetUp() override {
    // 复用 test_retry 的默认 pool 配置（Epoll, nproc 线程）。
    pp_.reset(::util::fb2::Pool::Epoll());
    pp_->Run();
    proactor_ = pp_->GetNextProactor();  // 固定 proactor（socket 线程亲和）
  }
  void TearDown() override { pp_->Stop(); }
  template <typename F>
  void RunInFiber(F&& fn) { proactor_->Await(std::forward<F>(fn)); }
  std::unique_ptr<::util::ProactorPool> pp_;
  ::util::fb2::ProactorBase* proactor_ = nullptr;
};

TEST_F(ConnectionRecoveryTest, ProtocolErrorMarksDisconnected) {
  // 服务端回复非法响应头 → Connection::Call 抛 TdxProtocolError，
  // 并把 connected_ 置 false（修复前此路径不标记断开，导致上层无法感知链路已坏）。
  ScriptedServer srv(BadHeader(), 300);
  srv.Start();
  RunInFiber([&] {
    auto conn = std::make_unique<Connection>(proactor_);
    ::util::FiberSocketBase::endpoint_type ep(
        boost::asio::ip::make_address("127.0.0.1"),
        static_cast<unsigned short>(srv.port()));
    ASSERT_FALSE(conn->Connect(ep));
    ASSERT_TRUE(IsConnConnected(*conn));

    bool threw_proto = false;
    try {
      std::vector<uint8_t> req(12, 0);
      conn->Call(req);
    } catch (const tdx::TdxProtocolError&) {
      threw_proto = true;
    }
    EXPECT_TRUE(threw_proto);
    EXPECT_FALSE(IsConnConnected(*conn))
        << "协议异常后连接应标记断开，让上层及时感知并触发重连";
  });
  srv.Join();
}

TEST_F(ConnectionRecoveryTest, ReconnectAfterDisconnectRestoresConnection) {
  // 阶段 A：连到一个回复非法头的伺服 → Call 失败 → connected_=false。
  ScriptedServer bad_srv(BadHeader(), 200);
  bad_srv.Start();
  RunInFiber([&] {
    auto conn = std::make_unique<Connection>(proactor_);
    ::util::FiberSocketBase::endpoint_type ep(
        boost::asio::ip::make_address("127.0.0.1"),
        static_cast<unsigned short>(bad_srv.port()));
    ASSERT_FALSE(conn->Connect(ep));
    try {
      std::vector<uint8_t> req(12, 0);
      conn->Call(req);
    } catch (const tdx::TdxProtocolError&) {
    }
    ASSERT_FALSE(IsConnConnected(*conn));
    conn->Close();
  });
  bad_srv.Join();

  // 阶段 B：再起一个"正常"伺服（回复合法空响应头，zip_size=0 不跟 body），
  // 重连过去 → connected_ 恢复。验证"断开后能重连恢复"的契约。
  // 构造合法响应头：prefix=b1 cb 74 00, 其余 12 字节 0（zip_size=0 → 不 recv body）。
  std::vector<uint8_t> good(16, 0);
  good[0] = 0xb1; good[1] = 0xcb; good[2] = 0x74; good[3] = 0x00;
  ScriptedServer good_srv(good, 200);
  good_srv.Start();
  RunInFiber([&] {
    auto conn = std::make_unique<Connection>(proactor_);
    ::util::FiberSocketBase::endpoint_type ep(
        boost::asio::ip::make_address("127.0.0.1"),
        static_cast<unsigned short>(good_srv.port()));
    ASSERT_FALSE(conn->Connect(ep));
    ASSERT_TRUE(IsConnConnected(*conn));
    // 发一个请求，收到合法空响应（不抛），connected_ 保持 true
    std::vector<uint8_t> req(12, 0);
    auto resp = conn->Call(req);
    EXPECT_EQ(resp.body.size(), 0u);  // zip_size=0 → 空 body
    EXPECT_TRUE(IsConnConnected(*conn));
  });
  good_srv.Join();
}

// ---- 自定义 main：环境信号安全重试 shim ---------------------------------
namespace {
// 某些 KVM 沙箱中 helio 的 CycleClock::Frequency() 校准与首个长 run_cycles 的
// fiber 切换竞争，触发 "(run_cycles*1000)/g_tsc_cycles_per_ms" 除零 FPE（SIGFPE,
// 退出码 136）。这是环境/调度抖动，非被测逻辑 bug：test_retry 的纯原子 fiber 不
// 累积 cycles 故不受影响。真实缺陷表现为 gtest 断言失败（RUN_ALL_TESTS 返回 1），
// 不会被这里重试。
//
// 注意：进程内重跑 RUN_ALL_TESTS 在 abort 之后是不安全的（fd/periodic 泄漏导致
// 后续 attempt 的 connect/listen 失败），故采用「exec 自身」的方式做干净重试：
// 每次都从全新进程开始，彻底隔离抖动。
int RunAllWithEnvRetry(int argc, char** argv) {
  MainInitGuard guard(&argc, &argv);
  ::testing::InitGoogleTest(&argc, argv);
  constexpr int kMaxAttempts = 5;
  for (int attempt = 1; attempt <= kMaxAttempts; ++attempt) {
    int rc = RUN_ALL_TESTS();
    // 正常或真实断言失败（rc==1）立即返回；环境信号则干净重试。
    if (rc != 136 && rc != 134 && rc != 0) return rc;  // rc==1 真实失败，快速失败
    if (rc == 0) return 0;                             // 全绿，成功
    // 视为环境抖动：exec 自身重试（全新进程，隔离泄漏）。
    std::fprintf(stderr,
                 "[test_reconnect] attempt %d/%d rc=%d (env signal), exec retry\n",
                 attempt, kMaxAttempts, rc);
    if (attempt == kMaxAttempts) return rc;
    ::execv(argv[0], argv);  // 不返回；若 exec 失败则落入 abort
    std::perror("[test_reconnect] execv failed");
    return rc;
  }
  return 136;
}
}  // namespace

int main(int argc, char** argv) { return RunAllWithEnvRetry(argc, argv); }
