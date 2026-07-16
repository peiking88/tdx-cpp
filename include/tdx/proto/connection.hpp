// helio FiberSocket 封装 + 通达信收发骨架。
// 镜像 helio/examples/echo_server.cc Driver（CreateSocket/Connect/WriteSome/Recv/Close）
// + opentdx client/baseStockClient.py:271-321 _send（封包→recv 16B 头→按 zipsize recv body→zlib 解压）。
//
// fiber 纪律：本类在 Proactor 线程/fiber 内使用，所有 I/O 经 helio fiber-aware socket，
// 不使用 std::mutex / std::this_thread::sleep_for。
//
// 注意：helio 的 util 命名空间是 ::util，本项目工具是 tdx::util——为避免在 tdx::proto 内
// 名字查找歧义，helio 类型一律用全限定 ::util::。
#pragma once

#include <memory>
#include <system_error>
#include <vector>

#include "util/fiber_socket_base.h"
#include "util/fibers/proactor_base.h"
#include "util/fibers/synchronization.h"  // fb2::Mutex：串行化并发 Call（请求 fiber vs 心跳 fiber）

#include "tdx/proto/frame.hpp"

namespace tdx::proto {

// 响应（解压后的 body + 响应头）
struct Response {
  ResponseHeader header;
  std::vector<uint8_t> body;  // 解压后
};

// 通达信 TCP 连接。绑定一个 Proactor；Call 可被同 Proactor 上多个 fiber 并发调用
// （请求 fiber + 心跳 fiber），由 call_mu_ 串行化整段 Send+Recv，避免协议错位。
// Connect/Close 为单 fiber 建立/拆除，不并发。
class Connection {
 public:
  using Socket = ::util::FiberSocketBase;
  using Endpoint = Socket::endpoint_type;  // = boost::asio::ip::tcp::endpoint

  explicit Connection(::util::fb2::ProactorBase* proactor);
  ~Connection();

  Connection(const Connection&) = delete;
  Connection& operator=(const Connection&) = delete;

  // 连接。返回 error_code（空=成功）。
  std::error_code Connect(const Endpoint& ep);

  // 发送请求封包 + 接收响应（封包→recv 16B 头→按 zipsize recv body→zlib 解压）。
  // 对齐 opentdx call/_send。失败抛 TdxConnectionError / TdxProtocolError。
  // 协议级失败（服务器返回 Bad message 等）标记 connected_=false，使上层感知链路已坏，
  // 触发重连；而非仅在 socket 级断连（Recv/Send 失败）才标记。
  // call_mu_ 串行化：同一 Connection 上的并发 Call 逐个完成完整往返，
  // 心跳帧不会插入业务请求的 Send/Recv 中段造成错位。
  Response Call(const std::vector<uint8_t>& request);

  bool IsConnected() const { return connected_; }

  // 裸 socket 访问（Heartbeat 等复用）
  Socket* socket() { return socket_.get(); }

  // 中止链路：Shutdown+Close fd，使任何挂起在 Recv/Send 的 fiber 尽快被唤醒报错。
  // 注意：不 reset socket_——若另有 fiber 持本对象（shared_ptr）正挂在 Recv，
  // socket_ 须保活至其返回；socket_ 由析构释放。避免 use-after-free（v0.21 修复）。
  void Close();

 private:
  // 循环发送直到全部写出
  std::error_code SendAll(const uint8_t* buf, std::size_t n);
  // 循环接收直到收满 n 字节
  std::error_code RecvExact(uint8_t* buf, std::size_t n);

  ::util::fb2::ProactorBase* proactor_;
  std::unique_ptr<Socket> socket_;
  // 串行化 Call：保证同一 socket 上一次只一个 Send+Recv 往返。
  // fiber 语义：持锁 fiber 在 Recv 挂起时锁仍保持，其他 Call fiber 协作等待。
  mutable ::util::fb2::Mutex call_mu_;
  bool connected_ = false;
};

}  // namespace tdx::proto
