// 股票代码格式校验。用于所有代码→TDengine 子表名拼接前，
// 防止特殊字符注入 SQL（TaosStmt 预编译因 libtaos 崩溃暂不可用）。
#pragma once

#include <string_view>

namespace tdx::util {

// 校验股票代码格式（纯数字 6 位）。TDX 协议返回的代码和 vipdoc 文件名
// 均为 6 位纯数字，作为 TDengine 子表名（如 q_600000）使用前必须校验。
inline constexpr bool IsValidCode(std::string_view code) {
  if (code.size() != 6) return false;
  for (char c : code) {
    if (c < '0' || c > '9') return false;
  }
  return true;
}

}  // namespace tdx::util
