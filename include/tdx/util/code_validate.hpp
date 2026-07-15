// 股票代码格式校验。用于所有代码→TDengine 子表名拼接前，
// 防止特殊字符注入 SQL（TaosStmt 预编译因 libtaos 崩溃暂不可用）。
#pragma once

#include <string_view>

namespace tdx::util {

// 校验股票代码格式（纯数字 4-8 位）。A 股 6 位（600000/000001）；港股 4-5 位（03690/00700/08001）、
// 恒指系列 8 位数字编码（800000）。作为 TDengine 子表名（q_<code>）使用前校验，防注入。
// 字母代码（HSI/HSTECH）返回 false——此类代码需调用方剥离或映射后再入库。
inline constexpr bool IsValidCode(std::string_view code) {
  if (code.size() < 4 || code.size() > 8) return false;
  for (char c : code) {
    if (c < '0' || c > '9') return false;
  }
  return true;
}

}  // namespace tdx::util
