# Feature: Phase 4 — v2 改进（并发批量 + 股票级断点续传 + DuckDB/Parquet 存储）

## Summary

补齐上游三短板：(1) **并发批量下载**（helio fiber 池 + `-n` 控并发，替代 tdxdata 纯串行）；(2) **股票级断点续传**（每股票完成即更新进度，崩溃可恢复，优于上游"只到天"）；(3) **存储查询**——Parquet 冷存储（系统 Arrow 23.0.1）+ DuckDB 嵌入式 SQL 查询层（直接 SELECT FROM parquet + 内存热表）。完成后支持 5000 股票并发抓取（-n 16）+ 崩溃恢复 + Parquet 落盘 + DuckDB SQL 回测查询。

> **修订（2026-06-23）**：去掉原 dragonfly 热缓存方案（独立 Redis 兼容服务，部署重），改为 **DuckDB + Parquet 组合**——DuckDB 嵌入式（进程内，零外部服务），直接 SQL 读写 Parquet，更适合 C++ 库。

## User Story

**作为** 全市场量化研究员，
**我想要** 一次命令并发抓取 5000 股票日 K 落盘 Parquet，崩溃可续传，并用 SQL 即席查询，
**以便** 高效构建回测数据集并直接 SQL 分析。

## Problem Statement

tdxdata 三个短板：① 全市场抓取纯串行；② 增量同步只到天不到股票；③ 存储全是桩。v2 用 helio 并发 + SyncState 批次 + Parquet（Arrow 写）/DuckDB（SQL 查询）解决。

## Solution Statement

`tdx::data::sync`（批次 schema + fb2::Mutex）+ `tdx::batch`（helio SimpleChannel worker 池）+ `tdx::storage`（Arrow Parquet writer/reader）+ `tdx::query`（DuckDB 嵌入式：SELECT FROM parquet + 内存热表）。CLI `batch-fetch -n N --resume <id> --parquet`。复用 Phase 1-3 全部。

## Metadata

| Field | Value |
|---|---|
| Type | ENHANCEMENT（v2 改进） |
| Complexity | HIGH（并发 fiber 池 + 断点续传 + Arrow Parquet + DuckDB 嵌入式） |
| Systems Affected | tdx::data::sync（增强）、tdx::batch（新）、tdx::storage（新）、tdx::query（新）、CLI、tests |
| Dependencies | 系统已装 Arrow 23.0.1 + Parquet 23.0.1（apt）；**DuckDB vendored 预编译 lib（libduckdb.so + duckdb.hpp，从镜像下，无外部服务）**；helio（内嵌） |
| Source PRD | `.claude/PRPs/prds/tdx-cpp.prd.md`（Phase 4） |
| Estimated Tasks | 8 |

---

## 关键技术决策（修订版）

| 决策 | 依据 | 影响 |
|---|---|---|
| **DuckDB 替代 dragonfly** | dragonfly 独立服务（部署重）；DuckDB 嵌入式（进程内，零服务），直接读写 Parquet | 去掉 RESP 客户端，用 DuckDB C++ API |
| **DuckDB vendored 预编译** | apt 无 libduckdb-dev；amalgamation 编译慢（duckdb.cpp ~50MB） | 下载 libduckdb-linux-amd64.zip（duckdb.hpp + libduckdb.so），vendored third_party/duckdb/ |
| **Parquet 用 Arrow 写，DuckDB 读** | Arrow 23.0.1 系统装；DuckDB 内置 Parquet reader | 写用 Arrow，查询用 DuckDB |
| 并发用 N worker + SimpleChannel | helio 无 Semaphore；SimpleChannel 满则背压 | 模式 A（非 5000 fiber） |
| Connection 每 worker 独立 | Connection 非线程安全（绑一 Proactor） | N worker = N Connection |
| SyncState 加 fb2::Mutex | nlohmann::json 非线程安全 | state_ 加锁 |
| 删 std::system("mkdir") | fork+exec 阻塞 Proactor | 改 std::filesystem |

---

## Patterns to Mirror

**PATTERN 1：DuckDB 嵌入式查询**（duckdb.hpp C++ API）
```cpp
#include <duckdb.hpp>
duckdb::DuckDB db(nullptr);          // 内存数据库（进程内，无服务）
duckdb::Connection con(db);
// 直接查 Parquet 文件（DuckDB 内置 Parquet reader）
auto r = con.Query("SELECT * FROM 'k.parquet' WHERE code = '600000' ORDER BY datetime");
// 内存热表（替代 dragonfly 热缓存——最新报价/订阅状态）
con.Query("CREATE MEMORY TABLE latest(code VARCHAR, price DOUBLE, ts BIGINT)");
con.Query("INSERT INTO latest VALUES ('600000', 10.5, 1719000000)");
// 写 Parquet（DuckDB 也可写）
con.Query("COPY (SELECT * FROM bars) TO 'out.parquet' (FORMAT PARQUET)");
```

**PATTERN 2：helio 并发池**（simple_channel.h + server_pool.cpp:45-60）
```cpp
util::fb2::SimpleChannel<Task> chan(N*2, 1);  // 背压
pb->Await([&] {
  std::vector<util::fb2::Fiber> workers;
  for (int i=0;i<N;++i) workers.push_back(util::MakeFiber([&,i]{
    tdx::proto::Connection conn(pb); conn.Connect(best);
    Task t; while (chan.Pop(t)) { /* fetch + parquet + sync */ }
  }));
  for (auto& c : codes) chan.Push({c});
  chan.StartClosing();
  for (auto& w : workers) w.Join();
});
```

**PATTERN 3：Arrow Parquet 写**（KLine 9 字段 → arrow::Table → WriteTable SNAPPY）

**PATTERN 4：断点续传 schema**（SyncEntry 加 batch_id/batch_status/retry_count）

**PATTERN 5：fiber 纪律**（沿用）+ nlohmann json 加 fb2::Mutex + std::filesystem

---

## Files to Create / Update

| 文件 | 动作 | 说明 |
|---|---|---|
| `include/tdx/data/sync_state.hpp` + `src/data/sync_state.cpp` | UPDATE | SyncEntry 加 batch 字段 + fb2::Mutex + 批次 API + std::filesystem |
| `include/tdx/data/storage/parquet_writer.hpp` + `.cpp` | CREATE | KLine→Parquet（Arrow） |
| `include/tdx/data/storage/parquet_reader.hpp` + `.cpp` | CREATE | 读末行 datetime 增量基准 |
| `src/data/storage/CMakeLists.txt` | CREATE | tdx_storage 链 Parquet::parquet_shared |
| `include/tdx/query/duckdb_query.hpp` + `src/query/duckdb_query.cpp` | CREATE | DuckDB 嵌入式：QueryParquet + 内存热表 |
| `third_party/duckdb/` | CREATE | vendored duckdb.hpp + libduckdb.so（镜像下载） |
| `CMakeLists.txt` | UPDATE | find_package(Arrow Parquet) + DuckDB IMPORTED |
| `include/tdx/batch/batch_fetch.hpp` + `src/batch/batch_fetch.cpp` | CREATE | N worker + SimpleChannel |
| `src/cli/main.cpp` | UPDATE | absl flags（-n/--resume/--parquet）+ batch-fetch/sql |
| `tests/test_sync4.cpp` + `test_parquet.cpp` + `test_duckdb.cpp` + `test_batch.cpp` | CREATE | 测试 |

---

## NOT Building

- **~~dragonfly 热缓存~~**（已修订去掉）—— 用 DuckDB 内存表替代
- **RESP 客户端 / dragonfly ProtocolClient** —— 不再需要
- **HTTP 爬虫** / **Python 绑定** —— Won't

---

## Step-by-Step Tasks

### Task 1: SyncState 增强（断点续传中枢）
- SyncEntry 加 batch_id/batch_status/retry_count/last_error；state_ 加 fb2::Mutex；StartBatch/IsCompletedInBatch/MarkStockComplete/MarkStockFailed；std::system → std::filesystem
- VALIDATE: 单元测试（批次/崩溃恢复/并发安全）

### Task 2: Parquet 冷存储（Arrow 写）
- parquet_writer（KLine→Arrow→Parquet SNAPPY）+ parquet_reader（末行 datetime）；CMake find_package(Arrow Parquet) + tdx_storage
- VALIDATE: 单元测试（写读 + schema 一致）

### Task 3: helio 并发批量（-n）
- batch_fetch（N worker fiber + SimpleChannel，每 worker 独立 Connection）
- VALIDATE: 单元测试（5000 股 mock + -n 1/4/16 + 背压）

### Task 4: 断点续传集成
- FetchHistory 增量（start 生效）+ 跳过 IsCompletedInBatch + --resume 崩溃恢复
- VALIDATE: 集成测试（中断→重启→跳过已完成）

### Task 5: DuckDB 嵌入式查询层（替代 dragonfly）
- vendored third_party/duckdb/（libduckdb-linux-amd64.zip 含 duckdb.hpp + libduckdb.so）；duckdb_query（QueryParquet "SELECT FROM parquet" + 内存热表 SetLatest/GetLatest）
- GOTCHA: DuckDB 内置 Parquet reader；嵌入式无服务；CMake IMPORTED libduckdb.so
- VALIDATE: 单元测试（SELECT FROM parquet + 内存表 + 写 Parquet）

### Task 6: 存储分层集成
- TdxData/FetchHistory 集成 Parquet 写（Arrow）+ DuckDB 查询 + 内存热表（替代 dragonfly）
- VALIDATE: 集成测试（并发抓取→Parquet→DuckDB 查询）

### Task 7: CLI 完整化
- absl flags（-n/--resume/--parquet）+ batch-fetch --stock-list + sql "SELECT ..."（DuckDB 即席查询）
- VALIDATE: CLI 烟雾测试

### Task 8: 测试 + 端到端验收
- test_sync4/parquet/duckdb/batch；端到端（batch-fetch 100 股 -n 8 --parquet --resume + DuckDB 查询）
- VALIDATE: 全套 + 并发性能（5000 股 -n 16 vs 串行）+ DuckDB 读 Parquet

---

## Acceptance Criteria

- [ ] ① 并发批量：5000 股 -n 16 显著快于串行（~10x）
- [ ] ② 股票级断点续传：中断重启跳过已完成
- [ ] ③ Parquet 落盘：DuckDB 可 SELECT FROM parquet，schema 一致
- [ ] ④ DuckDB 内存热表：最新报价可 INSERT/SELECT（替代 dragonfly）
- [ ] ⑤ fiber 纪律无违规（fb2::Mutex + std::filesystem）
- [ ] ⑥ 全套测试通过 + 覆盖率>80%

---

## Risks and Mitigations

| 风险 | 可能性 | 影响 | 缓解 |
|---|---|---|---|
| DuckDB vendored lib ABI | 中 | 链接错 | 固定 release 版本 + 测试 |
| DuckDB lib 下载（镜像） | 中 | 获取失败 | ghfast.top 镜像 + 校验 |
| SimpleChannel 并发模型 | 中 | 死锁/OOM | 仿 server_pool + 单测背压 |
| nlohmann 并发崩溃 | 高 | 数据损坏 | fb2::Mutex |
| DuckDB 内存表 vs Parquet 一致 | 中 | 查询差 | 内存表仅热数据，历史查 Parquet |

---

## Notes

- **修订要点**：dragonfly（独立服务）→ DuckDB（嵌入式），消除外部服务依赖。
- **DuckDB vs dragonfly**：DuckDB 嵌入式（进程内 SQL，无服务部署）+ 直接读 Parquet（回测 SQL）；dragonfly 独立 Redis 服务 + KV（不适合范围查询）。
- **实施顺序**：Task 1→3→2→5→4/6→7→8。
- **版本号**：Phase 4 → 0.4.0。

---

*下一步：评审本计划 → 运行 `/prp-implement .claude/PRPs/plans/phase4-v2-improvements.plan.md` 执行 Phase 4。*
