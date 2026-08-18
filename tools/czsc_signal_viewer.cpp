// czsc_signal_viewer：读 TDengine signals 表，展示分析结果。
//   czsc_signal_viewer 002515                 # 展示所有频率
//   czsc_signal_viewer 002515 --freq F5,D     # 指定频率
//   czsc_signal_viewer --list                 # 列出 signals 表所有标的
//   czsc_signal_viewer 002515 --json          # 输出 JSON
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

#include "czsc/io/signal_store.hpp"

#include "tdx/taos/taos_connection.hpp"  // 统一连接 RAII

namespace io = czsc::io;
namespace cz = czsc;

struct Args {
  std::string code;
  std::vector<std::string> freq_tags;  // 空=全
  bool list_only = false;
  bool json = false;
  io::LoaderConfig cfg;
};

Args ParseArgs(int argc, char** argv) {
  Args a;
  a.cfg = io::LoaderConfig::FromEnv();
  for (int i = 1; i < argc; ++i) {
    std::string s = argv[i];
    if (s == "--list") a.list_only = true;
    else if (s == "--json") a.json = true;
    else if (s == "--freq") {
      ++i;
      if (i >= argc) { std::fprintf(stderr, "--freq 缺值\n"); std::exit(2); }
      std::string v = argv[i];
      size_t p = 0;
      while (p < v.size()) {
        size_t c = v.find(',', p);
        a.freq_tags.push_back((c == std::string::npos) ? v.substr(p) : v.substr(p, c - p));
        if (c == std::string::npos) break;
        p = c + 1;
      }
    }
    else if (s == "--host") { ++i; if (i < argc) a.cfg.taos_host = argv[i]; }
    else if (s == "--user") { ++i; if (i < argc) a.cfg.taos_user = argv[i]; }
    else if (s == "--pass") { ++i; if (i < argc) a.cfg.taos_pass = argv[i]; }
    else if (s == "--db")   { ++i; if (i < argc) a.cfg.taos_db   = argv[i]; }
    else if (s == "--port") { ++i; if (i < argc) a.cfg.taos_port = static_cast<uint16_t>(std::atoi(argv[i])); }
    else if (s == "-h" || s == "--help") {
      std::printf("czsc_signal_viewer: 读取 signals 表\n"
                  "  --list                列出所有有信号代码\n"
                  "  <code>                指定标的（6 位或 sh/sz 前缀，如 sz002741）\n"
                  "  --freq F5,D           周期子集（空=全）\n"
                  "  --json                输出 JSON\n"
                  "  --host/--user/--pass/--db/--port\n");
      std::exit(0);
    }
    else if (s[0] != '-' && a.code.empty()) a.code = s;
  }
  return a;
}

std::string FreqTagFromName(const std::string& name) {
  if (name == "5m" || name == "5分钟") return "F5";
  if (name == "30m" || name == "30分钟") return "F30";
  if (name == "1d" || name == "日线" || name == "D") return "D";
  if (name == "W" || name == "week" || name == "周线") return "week";
  return name;
}

int main(int argc, char** argv) {
  Args args = ParseArgs(argc, argv);

  tdx::taos::TaosConnection conn_h{tdx::taos::TaosConfig{args.cfg.taos_host, args.cfg.taos_port, args.cfg.taos_user, args.cfg.taos_pass, args.cfg.taos_db}};
  ::TAOS* conn = conn_h.native();
  if (!conn) { std::fprintf(stderr, "连接 TDengine 失败\n"); return 1; }
  ::taos_query(conn, "USE tdx");

  if (args.list_only) {
    auto all = io::ListSignalCodes(conn);
    std::printf("signals 表中 %zu 只标的：\n", all.size());
    for (auto& [c, m] : all) std::printf("  %-8s  %s\n", c.c_str(), m.c_str());
      return 0;
  }

  if (args.code.empty()) {
    std::fprintf(stderr, "缺标的代码（或 --list）\n");
      return 1;
  }
  // ponytail: 复用 io::NormalizeCode（含 hk 前缀），拒绝非法格式。
  auto parsed = io::NormalizeCode(args.code);
  if (!parsed) {
    std::fprintf(stderr, "非法代码格式：%s（须 sh/sz/bj/hk 前缀，不接受裸码）\n", args.code.c_str());
      return 1;
  }
  std::string code = parsed->code;
  std::string mkt = parsed->mkt;

  // 自动发现频率
  auto all_codes = io::ListSignalCodes(conn);
  bool found_mkt = false;
  for (auto& [c, m] : all_codes) if (c == code && m == mkt) { found_mkt = true; break; }
  if (!found_mkt) {
    std::fprintf(stderr, "signals 表无 %s.%s\n", mkt.c_str(), code.c_str());
      return 1;
  }

  auto tags = args.freq_tags;
  if (tags.empty()) tags = {"F5", "F30", "D", "week"};

  if (args.json) {
    std::printf("{\n");
    for (size_t ti = 0; ti < tags.size(); ++ti) {
      std::string tag = FreqTagFromName(tags[ti]);
      auto rows = io::QuerySignals(conn, code, mkt, tag);
      std::printf("  \"%s\": [\n", tag.c_str());
      for (size_t i = 0; i < rows.size(); ++i) {
        std::string s;
        for (char ch : rows[i].sig) {
          if (ch == '"' || ch == '\\') s += '\\';
          s += ch;
        }
        std::printf("    {\"ts\":%lld,\"sig\":\"%s\"}%s\n",
                    (long long)rows[i].ts_ms, s.c_str(),
                    (i + 1 < rows.size() ? "," : ""));
      }
      std::printf("  ]%s\n", (ti + 1 < tags.size() ? "," : ""));
    }
    std::printf("}\n");
  } else {
    for (auto& tt : tags) {
      std::string tag = FreqTagFromName(tt);
      auto rows = io::QuerySignals(conn, code, mkt, tag);
      std::printf("=== %s%s [%s]  %d 条 ===\n", mkt.c_str(), code.c_str(), tag.c_str(), (int)rows.size());
      for (auto& r : rows) {
        char dbuf[32] = "-";
        if (r.ts_ms > 0) {
          time_t t = r.ts_ms / 1000;
          struct tm tmv;
          localtime_r(&t, &tmv);
          std::snprintf(dbuf, sizeof(dbuf), "%04d-%02d-%02d %02d:%02d:%02d",
                        tmv.tm_year+1900, tmv.tm_mon+1, tmv.tm_mday,
                        tmv.tm_hour, tmv.tm_min, tmv.tm_sec);
        }
        std::printf("  [%s] %s\n", dbuf, r.sig.c_str());
      }
    }
  }

  return 0;
}
