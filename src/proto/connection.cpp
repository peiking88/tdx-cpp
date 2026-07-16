#include "tdx/proto/connection.hpp"

#include <sys/socket.h>  // SHUT_RDWR

#include "io/io.h"  // io::Bytes / io::MutableBytes

#include "tdx/errors.hpp"
#include "tdx/util/zlib_wrap.hpp"  // tdx::util::zlib_inflate（注意是 tdx::util，不是 ::util）

namespace tdx::proto {

Connection::Connection(::util::fb2::ProactorBase* proactor) : proactor_(proactor) {
  socket_.reset(proactor_->CreateSocket());  // 对齐 echo_server.cc:405
  // 默认 8s 超时：覆盖 Connect + 后续所有 Send/Recv。
  // 防止服务器静默丢包（限流/防火墙 DROP）时 Call 永久 hang。
  socket_->set_timeout(8000);
}

Connection::~Connection() { Close(); }

std::error_code Connection::Connect(const Endpoint& ep) {
  if (!socket_) socket_.reset(proactor_->CreateSocket());
  auto ec = socket_->Connect(ep);  // echo_server.cc:417
  connected_ = !ec;
  return ec;
}

void Connection::Close() {
  // Shutdown+Close fd，使挂起在 Recv/Send 的 fiber 被唤醒报 connection_reset。
  // 不 reset socket_：上层 StdQuotes 持 shared_ptr，Reconnect 换 conn_ 后旧 conn 可能
  // 仍有在途 Call 持引用挂在 Recv——socket_ 须保活至该 Call 返回，由析构释放。
  // 旧实现此处 socket_.reset() 会立即销毁 socket，与挂起 Recv 竞争 → use-after-free。
  if (socket_) {
    std::error_code ec = socket_->Shutdown(SHUT_RDWR);  // echo_server.cc:551
    (void)ec;
    ec = socket_->Close();  // echo_server.cc:553
    (void)ec;
  }
  connected_ = false;
}

std::error_code Connection::SendAll(const uint8_t* buf, std::size_t n) {
  std::size_t sent = 0;
  while (sent < n) {
    auto es = socket_->WriteSome(::io::Bytes(buf + sent, n - sent));  // echo_server.cc:157
    if (!es) {
      if (Socket::IsConnClosed(es.error())) connected_ = false;
      return es.error();
    }
    if (es.value() == 0) {
      connected_ = false;
      return std::make_error_code(std::errc::connection_reset);
    }
    sent += es.value();
  }
  return {};
}

std::error_code Connection::RecvExact(uint8_t* buf, std::size_t n) {
  // 对齐 baseStockClient.py:312-317：循环 recv 直到收满 zipsize 字节。
  std::size_t got = 0;
  while (got < n) {
    auto es = socket_->Recv(::io::MutableBytes(buf + got, n - got), 0);  // echo_server.cc:441
    if (!es) {
      if (Socket::IsConnClosed(es.error())) connected_ = false;
      return es.error();
    }
    if (es.value() == 0) {  // 对端关闭
      connected_ = false;
      return std::make_error_code(std::errc::connection_reset);
    }
    got += es.value();
  }
  return {};
}

Response Connection::Call(const std::vector<uint8_t>& request) {
  // call_mu_ 串行化整段 Send+Recv：并发 Call（请求 vs 心跳）逐个完成完整往返，
  // 避免心跳帧插入业务请求的 Send/Recv 中段造成协议错位。持锁 fiber 在 Recv 挂起时
  // 锁保持，其他 fiber 协作等待——这是 fb2::Mutex 的设计语义。
  std::lock_guard<::util::fb2::Mutex> lk(call_mu_);
  if (!connected_) throw TdxConnectionError("not connected");

  // 1. 发送整个封包（对齐 opentdx client.send(data)）
  if (auto ec = SendAll(request.data(), request.size())) {
    throw TdxConnectionError("send failed: " + ec.message());
  }

  // 2. recv 16 字节响应头
  uint8_t head_buf[kRspHeaderLen];
  if (auto ec = RecvExact(head_buf, kRspHeaderLen)) {
    throw TdxConnectionError("recv header failed: " + ec.message());
  }

  Response resp;
  try {
    resp.header = parse_response_header(head_buf);  // 校验 prefix（D8）
  } catch (...) {
    connected_ = false;  // 协议异常标记断开，让上层感知链路已坏并触发重连
    throw;
  }

  // 3. 按 zipsize 循环 recv body（baseStockClient.py:312-317）
  std::vector<uint8_t> raw(resp.header.zip_size);
  if (resp.header.zip_size > 0) {
    if (auto ec = RecvExact(raw.data(), resp.header.zip_size)) {
      throw TdxConnectionError("recv body failed: " + ec.message());
    }
  }

  // 4. zlib 解压（zipsize != unzip_size，baseStockClient.py:318-319）
  if (need_unzip(resp.header)) {
    auto inflated = tdx::util::zlib_inflate(raw.data(), raw.size());
    if (inflated.empty() && !raw.empty()) {
      connected_ = false;  // 解压失败 → 损坏帧，标记断开
      throw TdxProtocolError("zlib inflate failed");
    }
    resp.body = std::move(inflated);
  } else {
    resp.body = std::move(raw);
  }

  return resp;
}

}  // namespace tdx::proto
