// server_pool 单元测试：并发测速 + 选最优（用本地 listening socket，不依赖真服）。
#include <gtest/gtest.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <memory>

#include "base/init.h"
#include "util/fibers/pool.h"

#include "tdx/proto/server_pool.hpp"

using namespace tdx::proto;

class ServerPoolTest : public ::testing::Test {
 protected:
  void SetUp() override {
    pp_.reset(::util::fb2::Pool::Epoll());
    pp_->Run();
  }
  void TearDown() override { pp_->Stop(); }

  std::unique_ptr<::util::ProactorPool> pp_;
};

// 起一个本地 listening socket（仅 bind+listen，不 accept），返回分配端口。
static uint16_t MakeLocalListener() {
  int srv = ::socket(AF_INET, SOCK_STREAM, 0);
  EXPECT_GE(srv, 0);
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  socklen_t addrlen = sizeof(addr);
  EXPECT_EQ(::bind(srv, reinterpret_cast<sockaddr*>(&addr), addrlen), 0);
  EXPECT_EQ(::listen(srv, 5), 0);
  ::getsockname(srv, reinterpret_cast<sockaddr*>(&addr), &addrlen);
  uint16_t port = ntohs(addr.sin_port);
  // srv 故意不关闭：保持 listening 以供测速 connect
  return port;
}

TEST_F(ServerPoolTest, SelectsReachableHost) {
  uint16_t live_port = MakeLocalListener();

  ServerPool sp(pp_.get());
  std::vector<ServerInfo> hosts = {
      {"unreachable", "127.0.0.1", 1},     // connection refused
      {"local_live", "127.0.0.1", live_port},  // 可连
  };

  auto* pb = pp_->GetNextProactor();
  auto best = pb->Await([&] { return sp.SelectBest(hosts); });

  ASSERT_TRUE(best.has_value());
  EXPECT_EQ(best->port, live_port);  // 应选可连的 local_live
}

TEST_F(ServerPoolTest, AllUnreachableReturnsNullopt) {
  ServerPool sp(pp_.get());
  std::vector<ServerInfo> hosts = {
      {"dead1", "127.0.0.1", 1},
      {"dead2", "127.0.0.1", 2},
  };

  auto* pb = pp_->GetNextProactor();
  auto best = pb->Await([&] { return sp.SelectBest(hosts); });
  EXPECT_FALSE(best.has_value());
}

int main(int argc, char** argv) {
  MainInitGuard guard(&argc, &argv);
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
