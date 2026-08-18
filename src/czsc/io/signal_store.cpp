// 信号持久化层：TDengine 稳定子表 signals，覆盖式写入。
// signals(ts TIMESTAMP, sig VARCHAR(255)) TAGS (code VARCHAR(10), market VARCHAR(2), freq VARCHAR(8))
#include "czsc/io/signal_store.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <set>
#include <sstream>
#include <string_view>

#include "czsc/types/enums.hpp"

namespace czsc::io {

namespace {
int64_t ExecAffected(TAOS* conn, const std::string& sql) {
  TAOS_RES* res = ::taos_query(conn, sql.c_str());
  int code = ::taos_errno(res);
  if (code != 0) {
    std::fprintf(stderr, "TDengine error [%d]: %s\n  SQL(len=%zu)\n",
                 code, ::taos_errstr(res), sql.size());
    ::taos_free_result(res);
    return -1;
  }
  int64_t n = ::taos_affected_rows(res);
  ::taos_free_result(res);
  return n;
}
}  // namespace

std::string EscapeSql(std::string_view s) {
  std::string out;
  out.reserve(s.size() + 4);
  for (char ch : s) {
    if (ch == '\'') out += "''";
    else out += ch;
  }
  return out;
}

std::string FreqTag(Freq f) {
  switch (f) {
    case Freq::k5Min:  return "F5";
    case Freq::k30Min: return "F30";
    case Freq::kDay:   return "D";
    case Freq::kWeek:  return "week";
    default:           return "D";
  }
}

// ponytail: 统一入口；必须 2 位前缀（sh/sz/bj/hk）+ 5~6 位数字，裸码一律拒绝
//   （A 股 6 位与港股 5 位首字符重叠，无法区分）。8 位识别 sh/sz/bj，7 位识别 hk+5 位。
std::optional<CodeKey> NormalizeCode(std::string_view raw) {
  if (raw.empty()) return std::nullopt;
  std::string s(raw);
  for (auto& ch : s) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));

  // 2 位市场前缀 + 代码（sh/sz/bj/hk）
  if (s.size() >= 7 && (s[0] == 's' || s[0] == 'b' || s[0] == 'h')) {
    std::string pre = s.substr(0, 2);
    if (pre == "sh" || pre == "sz" || pre == "bj" || pre == "hk") {
      std::string num = s.substr(2);
      if (num.size() >= 5 && num.size() <= 6 &&
          std::all_of(num.begin(), num.end(),
                      [](char c) { return c >= '0' && c <= '9'; })) {
        CodeKey k;
        k.mkt = pre;
        k.code = num;
        k.display = pre + num;
        return k;
      }
    }
    return std::nullopt;
  }

  // 裸数字码（无前缀）一律拒绝
  return std::nullopt;
}

void EnsureSignalTable(TAOS* conn) {
  ExecAffected(conn,
      "CREATE STABLE IF NOT EXISTS signals ("
      "ts TIMESTAMP, sig VARCHAR(255)) "
      "TAGS (code VARCHAR(10), market VARCHAR(2), freq VARCHAR(8))");
}

int64_t RewriteSignals(TAOS* conn, const std::string& code,
                       const std::string& mkt, const std::string& freq_tag,
                       const std::vector<czsc::Signal>& signals, int64_t ts_ms) {
  if (code.empty() || signals.empty()) return 0;
  std::string tb = "s_" + mkt + code + "_" + freq_tag;
  if (ExecAffected(conn, "DROP TABLE IF EXISTS " + tb) < 0) return -1;

  constexpr int kBatch = 500;
  int64_t written = 0;
  const std::string esc_mkt = EscapeSql(mkt);
  const std::string esc_code = EscapeSql(code);
  const std::string esc_freq = EscapeSql(freq_tag);

  for (size_t i = 0; i < signals.size(); i += kBatch) {
    size_t end = std::min(i + kBatch, signals.size());
    std::ostringstream sql;
    sql << "INSERT INTO " << tb << " USING signals TAGS('"
        << esc_code << "','" << esc_mkt << "','" << esc_freq << "') VALUES";
    int n_added = 0;
    for (size_t j = i; j < end; ++j) {
      std::string sig = EscapeSql(signals[j].to_string());
      if (sig.empty()) continue;
      if (n_added++ > 0) sql << ' ';
      // ponytail: signals TIMESTAMP 作主键，同 ms 会覆盖；按信号序号 +1 ms 散开。
      int64_t row_ms = ts_ms + static_cast<int64_t>(j);
      char row[512];
      std::snprintf(row, sizeof(row), "(%lld,'%s')", (long long)row_ms, sig.c_str());
      sql << row;
    }
    if (n_added == 0) continue;
    int64_t n = ExecAffected(conn, sql.str());
    if (n < 0) return -1;
    written += n_added;
  }
  return written;
}

std::vector<SignalRow> QuerySignals(TAOS* conn, const std::string& code,
                                    const std::string& mkt, const std::string& freq_tag) {
  std::vector<SignalRow> out;
  std::string tb = "s_" + EscapeSql(mkt) + EscapeSql(code) + "_" + EscapeSql(freq_tag);
  // 表不存在时跳过（DTEx 子表名下划线不可能出现在合法 code/freq 中）。
  std::string sql = "SELECT ts, sig FROM " + tb + " ORDER BY ts";
  TAOS_RES* res = ::taos_query(conn, sql.c_str());
  if (!res || ::taos_errno(res) != 0) {
    ::taos_free_result(res);
    return out;
  }
  TAOS_ROW row;
  while ((row = ::taos_fetch_row(res))) {
    SignalRow r;
    if (row[0]) r.ts_ms = *static_cast<const int64_t*>(row[0]);
    if (row[1]) r.sig = std::string(reinterpret_cast<const char*>(row[1]));
    out.push_back(std::move(r));
  }
  ::taos_free_result(res);
  return out;
}

std::vector<std::pair<std::string, std::string>> ListSignalCodes(TAOS* conn) {
  std::vector<std::pair<std::string, std::string>> out;
  TAOS_RES* res = ::taos_query(conn, "SELECT DISTINCT code, market FROM signals");
  if (!res || ::taos_errno(res) != 0) {
    ::taos_free_result(res);
    return out;
  }
  std::set<std::pair<std::string, std::string>> uniq;
  TAOS_ROW row;
  // 列实际长度（libtaos row[i] 指针指向固定宽缓冲，不一定 \0 结尾，必须用 lengths 定界）。
  int num_cols = ::taos_num_fields(res);
  while ((row = ::taos_fetch_row(res))) {
    if (!row[0]) continue;
    int* lens = (num_cols > 0) ? ::taos_fetch_lengths(res) : nullptr;
    std::string code = row[0] ? std::string(reinterpret_cast<const char*>(row[0]),
                                            num_cols > 0 && lens ? lens[0] : 0) : "";
    std::string mkt = (num_cols > 1 && row[1]) ?
                      std::string(reinterpret_cast<const char*>(row[1]),
                                  lens ? lens[1] : 0) : "";
    uniq.emplace(std::move(code), std::move(mkt));
  }
  ::taos_free_result(res);
  out.assign(uniq.begin(), uniq.end());
  return out;
}

}  // namespace czsc::io
