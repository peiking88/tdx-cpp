// tdx 协议常量与枚举。逐字对齐 opentdx/const.py（数值不可臆测）。
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

namespace tdx {

// 市场（opentdx const.py:195-199 MARKET）
enum class Market : uint16_t {
  SZ = 0,  // 深圳
  SH = 1,  // 上海
  BJ = 2,  // 北证
};

// K 线周期（opentdx const.py:281-294 PERIOD，逐字移植数值）。
// 注意：MIN_1=7、MINS=8、DAYS=9 与部分文档表述不同，以源码为准。
enum class Period : uint16_t {
  MIN_5     = 0,   // 5 分钟
  MIN_15    = 1,   // 15 分钟
  MIN_30    = 2,   // 30 分钟
  HOUR_1    = 3,   // 60 分钟
  DAILY     = 4,   // 日
  WEEKLY    = 5,   // 周
  MONTHLY   = 6,   // 月
  MIN_1     = 7,   // 1 分钟
  MINS      = 8,   // 多分钟（如 10 分钟）
  DAYS      = 9,   // 多日（如 45 日）
  QUARTERLY = 10,  // 季
  YEARLY    = 11,  // 年
  SECONDS   = 13,  // 多秒（如 5 秒）
};

// 复权（opentdx const.py:296-299 ADJUST）
enum class Adjust : uint8_t {
  NONE = 0,  // 不复权
  QFQ  = 1,  // 前复权
  HFQ  = 2,  // 后复权
};

// A 股分类（opentdx const.py:207-240 CATEGORY，Phase 1 用到的子集）
enum class Category : uint32_t {
  SH        = 0,
  SZ        = 2,
  A         = 6,
  B         = 7,
  KCB       = 8,      // 科创板
  BJ        = 12,     // 北证 A
  CYB       = 14,     // 创业板
  BOARD_ALL = 10000,  // 全部板块
  HGT       = 0x2af9, // 沪股通
  SGT       = 0x2b01, // 深股通
  ETF       = 0x2afd, // ETF 基金
  LOF       = 0x2b04, // LOF 基金
  ZS        = 0x2b2c, // 沪深系列指数
};

// 默认端口（opentdx const.py）
inline constexpr uint16_t kStdPort = 7709;  // 标准行情（A 股）
inline constexpr uint16_t kExPort  = 7727;  // 扩展行情（期货/港美股）
// SP/MAC 高级行情（板块/资金流）走 mac_hosts，同样 7709。

// 时间常量（opentdx，单位秒）
inline constexpr int kHeartbeatIntervalSec = 15;  // 心跳间隔（heartbeat.py DEFAULT_HEARTBEAT_INTERVAL=15.0）
inline constexpr int kHeartbeatMaxIdle     = 20;  // 连续 20 次纯心跳无业务请求则断开
inline constexpr int kConnectTimeoutSec    = 5;   // 连接超时 time_out=5

// 单次请求上限（opentdx）
inline constexpr int kKlineMaxCount = 800;   // K 线单次上限
inline constexpr int kTickMaxCount  = 2000;  // 分笔单次上限

// 价格/金额缩放（opentdx quotationClient.py:24-30）
inline constexpr double kPriceScale  = 100.0;  // 实时行情价格 /100
inline constexpr double kAmountScale = 100.0;  // 金额 *100

// 解析带市场前缀的代码：sh000001 → (SH,"000001")；sz000001 → (SZ,"000001")；
// bj430047 → (BJ,"430047")。无前缀返回 code 空（项目规范要求 code 必须带市场前缀，
// 不推断市场——歧义 code 如 000001 须显式前缀区分 SH 上证指数 / SZ 平安银行）。
// 调用方须检查 code.empty() 报错。
inline std::pair<Market, std::string> ParseMarketCode(std::string_view s) {
  if (s.size() >= 8) {
    auto pre = s.substr(0, 2);
    if (pre == "sh") return {Market::SH, std::string(s.substr(2))};
    if (pre == "sz") return {Market::SZ, std::string(s.substr(2))};
    if (pre == "bj") return {Market::BJ, std::string(s.substr(2))};
  }
  return {Market::SH, ""};  // 无前缀：code 空表示无效
}


// ============ Phase 2：扩展行情 + SP/MAC 枚举 ============

// 扩展市场（opentdx const.py:491-544 EX_MARKET）。值域 1-102，与标准 MARKET(0-2) 独立。
enum class ExMarket : uint8_t {
  TempStock = 1,
  ZzFuturesOption = 4, DlFuturesOption = 5, ShFuturesOption = 6,
  CffexOption = 7, ShStockOption = 8, SzStockOption = 9,
  BasicFx = 10, CrossFx = 11, IntlIndex = 12,
  ComexFutures = 16, NymexFutures = 17, CbotFutures = 18,
  HkFinancialFutures = 23, HkFinancialOptions = 24,
  HkStockFutures = 25, HkStockOptions = 26, HkIndex = 27,
  ZzFutures = 28, DlFutures = 29, ShFutures = 30,
  HkMainBoard = 31,
  OpenEndFund = 33, MonetaryFund = 34, MacroIndicator = 38,
  FuturesIndex = 42, BToH = 43, Neeq = 44, ShGold = 46,
  CffexFutures = 47, HkGem = 48, HkFund = 49,
  TreasuryValuation = 54, SunshinePrivateFund = 56,
  BrokerCollectiveFinance = 57, BrokerMonetaryFinance = 58,
  MainFuturesContract = 60, CsiIndex = 62,
  GzArbitrageFutures = 65, GzFutures = 66, GzOptions = 67,
  RiskControlIndex = 68, HuazhengIndex = 69, ExtendedSectorIndex = 70,
  HkStockGgt = 71, GeStock = 73, UsStock = 74, SgStock = 78,
  MoneyMarket = 91, FundValuation = 93, HkDarkPool = 98,
  CodeMirror = 100, SzseIndex = 102,
};

// ============ 港股分类（按代码长度 + 前缀推断 ExMarket）============
// 与 mootdx.quotes.validate 同哲学：不允许裸 code 推断 A/HK 归属，开发者须显式
// 传 ExMarket（即 call site 决定走 HK 还是 A 股）。本函数仅做「HK 业务分类」：
//   前提：调用方已确认此 code 是港股（例：fetch-quotes --quote_hk 批量用户输入、
//         BarsAuto 中已按前缀分流）；本函数只做 code → ExMarket 分桶，不做 HK/A 股判断。
// 规则（对齐 opentdx EX_CATEGORY / mootdx 业务 ID）：
//   5 位数字（03690/00700 等）→ HkMainBoard=31
//   4 位数字首位 0/8（08001 等 GEM）→ HkGem=48，其他 4 位 → HkMainBoard=31
//   8 位数字（800000=tcp HSI）→ HkIndex=27
//   字母开头（HSI/HSTECH 等）→ HkIndex=27
// 其他长度/前缀不视为合法 HK 代码，返回基值 HkMainBoard（调用方不应传入）。
inline ExMarket ClassifyHk(std::string_view code) {
  if (code.empty()) return ExMarket::HkMainBoard;
  bool all_digit = true;
  for (char c : code) { if (c < '0' || c > '9') { all_digit = false; break; } }
  if (!all_digit) return ExMarket::HkIndex;  // 字母开头（HSI/HSTECH）→ 恒指
  switch (code.size()) {
    case 4:
      // GEM：首位 0/8（实际范围 30 年内仍保持 4 位创业板编码）
      return (code[0] == '0' || code[0] == '8') ? ExMarket::HkGem : ExMarket::HkMainBoard;
    case 5:
      return ExMarket::HkMainBoard;
    case 8:  // 800000/800001 等恒指系列（TDX 内部数字编码）
      return ExMarket::HkIndex;
    default:
      return ExMarket::HkMainBoard;
  }
}

// 「这个 code（可能带 sh/sz/bj 前缀）是否港股？」——供 fetch-quotes --quote_hk 批量分流用。
// 保守规则：① 已带 sh/sz/bj 前缀 → A 股（由 ParseMarketCode 处理）；② 剥前缀后按
// ClassifyHk 业务规则：4/5 位纯数字、字母开头视为 HK 代码。
// 6-8 位纯数字不识别为 HK（归 A 股/配售/新股认购代码）。返回 false 表示非 HK。
inline bool IsHkCode(std::string_view code) {
  if (code.size() >= 8) {
    auto pre = code.substr(0, 2);
    if (pre == "sh" || pre == "sz" || pre == "bj") return false;  // 显式 A 股前缀
  }
  bool all_digit = true;
  for (char c : code) { if (c < '0' || c > '9') { all_digit = false; break; } }
  if (!all_digit) {
    // 字母开头 → HK 指数（HSI/HSTECH）；其他字符（符号等）→ 非 HK
    return (code[0] >= 'A' && code[0] <= 'Z') || (code[0] >= 'a' && code[0] <= 'z');
  }
  return code.size() == 4 || code.size() == 5;
}

// 显式分流：调用方声明「这批 code 是 HK」，返回各 code 归属的 ExMarket。
// 与 IsHkCode 互补：IsHkCode 是保守自动推断（6-8 位数字不识别，避免与 A 股冲突），
// ClassifyHkExplicit 是调用方显式指定「按 HK 业务规则分桶」——用于已知是 HK 的场景
// （如用户显式传 --quote_hk 的 code 列表、或 BarsAuto(code, ExMarket::Hk*) 调用）。
// 规则：
//   5 位数字 → HkMainBoard；4 位数字首位 0/8 → HkGem，其他 4 位 → HkMainBoard；
//   字母开头（HSI/HSTECH/800000 等在 BarsAuto 调用前已被 IsHkCode 筛掉）→ HkIndex。
// 注意：TDX 恒指编码 800000 是 6 位数字，与 A 股长度重叠，IsHkCode 保守拒绝——
//       调用 BarsAuto(800000) 走 A 股 StdQuotes（返回空，因 A 股无此 code）；
//       业务上如需采 HSI 须显式 BarsAuto("HSI") 或 BarsAuto("800000", HkIndex)。
inline ExMarket ClassifyHkExplicit(std::string_view code) {
  if (code.empty()) return ExMarket::HkMainBoard;
  bool all_digit = true;
  for (char c : code) { if (c < '0' || c > '9') { all_digit = false; break; } }
  if (!all_digit) return ExMarket::HkIndex;  // 字母开头（HSI/HSTECH）→ 恒指
  switch (code.size()) {
    case 4:
      return (code[0] == '0' || code[0] == '8') ? ExMarket::HkGem : ExMarket::HkMainBoard;
    case 5:
      return ExMarket::HkMainBoard;
    default:
      return ExMarket::HkMainBoard;
  }
}

// 扩展类别（opentdx const.py:243-255 EX_CATEGORY）
enum class ExCategory : uint32_t {
  Hk = 0x001f, HkGem = 0x0030, Ggt = 0x0047, Us = 0x004a,
  Hsi = 0x2ee1, Hshc = 0x2ee2, Hsgq = 0x2ee4, Hsgz = 0x2ee7,
  Hskj = 0x2eec, Uszgg = 0x32c9, Uszm = 0x32ca,
};

// 板块类型（opentdx const.py:470-481 BOARD_TYPE）
enum class BoardType : uint8_t {
  Hy = 0, Hy2 = 1, Gn = 3, Fg = 4, Dq = 5, Other = 6,
  YjLevel1 = 7, YjLevel2 = 8, YjLevel3 = 9, All = 255,
};

// 本地板块文件类型（opentdx const.py:464-468 BLOCK_FILE_TYPE）
enum class BlockFileType : uint8_t {
  Default, Zs, Fg, Gn,
};
inline constexpr const char* BlockFileName(BlockFileType t) {
  switch (t) {
    case BlockFileType::Default: return "block.dat";
    case BlockFileType::Zs: return "block_zs.dat";
    case BlockFileType::Fg: return "block_fg.dat";
    case BlockFileType::Gn: return "block_gn.dat";
  }
  return "block.dat";
}

// 排序类型（opentdx const.py:309-401 SORT_TYPE，子集）
enum class SortType : uint16_t {
  Code = 0x00, Name = 0x01, PreClose = 0x02, Open = 0x03,
  High = 0x04, Low = 0x05, Price = 0x06, Bid = 0x07, Ask = 0x08,
  Volume = 0x09, TotalAmount = 0x0a, LastVolume = 0x0b,
  Change = 0x0c, ChangePct = 0x0e, AmplitudePct = 0x0f, Avg = 0x10,
  PeDynamic = 0x11, VolRatio = 0x23, TurnoverRate = 0x24,
  Activity = 0x2f,
};

// 排序方向（opentdx const.py:459-462 SORT_ORDER）
enum class SortOrder : uint8_t {
  None = 0, Desc = 1, Asc = 2,
};

// ============ Phase 3：交易时段常量（对齐 tdxdata base.py:62-63）============
// A 股交易时段（分钟数，0:00 起）：上午 9:30-11:30，下午 13:00-15:00
inline constexpr int kMorningOpenMin = 570;    // 09:30
inline constexpr int kMorningCloseMin = 690;   // 11:30
inline constexpr int kAfternoonOpenMin = 780;  // 13:00
inline constexpr int kAfternoonCloseMin = 900; // 15:00
inline constexpr int kSessionSplitMin = 720;   // 12:00（base.py:68 切上下午分支用，非 11:30）

}  // namespace tdx
