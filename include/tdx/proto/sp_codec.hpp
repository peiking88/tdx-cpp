// SP/MAC 协议工具函数。对齐 opentdx utils/help.py:55-89。
//   combine_to_datetime：ymd(YYYYMMDD 整数) + 当日秒数 → epoch
//   exchange_board_code：板块可视 id（880xxx/HK/US/399/899/000）→ 系统真实 board_code
#pragma once

#include <cstdint>
#include <string_view>

namespace tdx::proto {

// 由 ymd(YYYYMMDD 整数) + date_num(当日秒数) 构造 epoch seconds（CST）。
// format_tdx_time=true 且 0<=hour<=5 时 +1 天（美股/期货日期偏移，help.py:61-62）。
int64_t combine_to_datetime(int ymd, int date_num, bool format_tdx_time = false);

// 板块可视 id → 系统真实 board_code（help.py:66-89）：
//   USxxxx → 30000+num, HKxxxx → 20000+num, 000xxx → 31000+num,
//   399xxx(6位) → 30000+(num-399000), 899xxx(6位) → 32000+(num-899000),
//   88xxxx(6位) → 20000+(num-880000), 其他 → num 本身
int exchange_board_code(std::string_view board_symbol);

}  // namespace tdx::proto
