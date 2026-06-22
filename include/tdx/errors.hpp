// tdx 异常层次。对齐 opentdx errors（TdxConnectionError/TdxFunctionCallError）+ tdxdata errors。
#pragma once

#include <stdexcept>
#include <string>

namespace tdx {

class TdxError : public std::runtime_error {
 public:
  explicit TdxError(const std::string& msg) : std::runtime_error(msg) {}
};

// 协议格式错误（响应 prefix 校验失败、zlib 损坏、字段截断等）
class TdxProtocolError : public TdxError {
 public:
  explicit TdxProtocolError(const std::string& msg) : TdxError(msg) {}
};

// 连接错误（断开、超时、不可达）
class TdxConnectionError : public TdxError {
 public:
  explicit TdxConnectionError(const std::string& msg) : TdxError(msg) {}
};

// 功能调用错误（send/recv 失败、响应格式错误）
class TdxFunctionCallError : public TdxError {
 public:
  explicit TdxFunctionCallError(const std::string& msg) : TdxError(msg) {}
};

}  // namespace tdx
