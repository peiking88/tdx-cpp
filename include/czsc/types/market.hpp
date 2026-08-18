// 市场类型：sh / sz / bj / hk
// ponytail: 市场仅由显式 2 位前缀决定，绝不靠代码数字反推（A 股 6 位与港股 5 位
//           数字首字符有重叠，如 00700 会被错判），故所有标的代码必须带 sh/sz/bj/hk 前缀。
#pragma once

#include <cctype>
#include <string_view>

namespace czsc {

enum class Market : uint8_t { kSh, kSz, kBj, kHk };

// Market → 2 位小写前缀 "sh" / "sz" / "bj" / "hk"
inline const char* MarketToPrefix(Market m) {
  switch (m) {
    case Market::kSh: return "sh";
    case Market::kSz: return "sz";
    case Market::kBj: return "bj";
    case Market::kHk: return "hk";
  }
  return "sh";
}

// 2 位前缀（大小写不敏感）→ Market；无法识别时回退 kSh。
inline Market PrefixToMarket(std::string_view s) {
  if (s.size() != 2) return Market::kSh;
  char a = static_cast<char>(std::tolower(static_cast<unsigned char>(s[0])));
  char b = static_cast<char>(std::tolower(static_cast<unsigned char>(s[1])));
  if (a == 's' && b == 'h') return Market::kSh;
  if (a == 's' && b == 'z') return Market::kSz;
  if (a == 'b' && b == 'j') return Market::kBj;
  if (a == 'h' && b == 'k') return Market::kHk;
  return Market::kSh;
}

}  // namespace czsc
