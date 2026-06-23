# Feature: Phase 4 — v2 改进（并发批量 + 股票级断点续传 + DuckDB 存储）

## Summary

补齐上游三短板：(1) **并发批量下载**（helio fiber 池 + `-n`）；(2) **股票级断点续传**（每股票完成即更新进度，崩溃可恢复）；(3) **存储查询**——DuckDB 嵌入式 SQL 引擎统一覆盖 Parquet 读写 + 即席查询 + 内存热表（**去除 Arrow 依赖**，Parquet 读写全部走 DuckDB）。第三方依赖统一收纳到 `external/`（helio + duckdb）。

> **修订历史**：
> - 2026-06-23 v1：dragonfly 热缓存 + Parquet(Arrow)
> - 2026-06-23 v2：dragonfly → DuckDB + Parquet(Arrow)
> - 2026-06-23 **v3（本版）**：**去除 Arrow 依赖**（Parquet 读写全用 DuckDB 内置能力）+ **第三方依赖统一 external/**（helio + duckdb）

## User Story

**作为** 全市场量化研究员，
**我想要** 一次命令并发抓取 5000 股票日 K 落盘 Parquet，崩溃可续传，并用 SQL 即席查询，
**以便** 高效构建回测数据集并直接 SQL 分析。

## Solution Statement

`tdx::data::sync`（批次 schema + fb2::Mutex）+ `tdx::batch`（helio SimpleChannel worker 池）+ `tdx::query`（DuckDB 嵌入式：Parquet 读写 COPY TO/SELECT FROM + SQL 查询 + 内存热表）。第三方依赖统一 `external/`（helio symlink + duckdb vendored）。CLI `batch-fetch -n N --resume <id> --parquet`。复用 Phase 1-3。

## Metadata

| Field | Value |
|---|---|
| Type | ENHANCEMENT（v2 改进） |
| Complexity | HIGH（external/ 重构 + 并发 + 断点续传 + DuckDB 嵌入式） |
| Systems Affected | 项目根（external/ + CMakeLists + .gitignore）、tdx::data::sync、tdx::batch、tdx::query、CLI、tests |
| Dependencies | **external/helio**（symlink 到 ~/framework/dragonfly/helio）+ **external/duckdb**（vendored libduckdb.so + duckdb.hpp，镜像下载）；**无 Arrow 依赖** |
| Source PRD | `.claude/PRPs/prds/tdx-cpp.prd.md`（Phase 4） |
| Estimated Tasks | 8 |

---

## 关键技术决策（v3 修订）

| 决策 | 依据 | 影响 |
|---|---|---|
| **去除 Arrow 依赖** | DuckDB 内置 Parquet reader/writer（COPY TO / SELECT FROM parquet），Arrow 冗余 | 删 find_package(Arrow Parquet)；Parquet 读写全走 DuckDB |
| **第三方依赖统一 external/** | helio 现散在 HELIO_PATH（~/framework/...）；duckdb vendored 散放；统一 external/ 便于管理 + gitignore | external/helio（symlink）+ external/duckdb（vendored），.gitignore external/ |
| **DuckDB 覆盖 Parquet 读写** | DuckDB `COPY tbl TO 'x.parquet' (FORMAT PARQUET)` 写 + `SELECT FROM 'x.parquet'` 读 | 替代 Arrow writer/reader（原 Task 2 删除，并入 DuckDB Task） |
| **helio 用 symlink 进 external/** | helio ~100MB+，vendored 复制不现实；symlink external/helio → ~/framework/dragonfly/helio | HELIO_PATH 默认 external/helio |
| 并发用 N worker + SimpleChannel | helio 无 Semaphore；SimpleChannel 背压 | 模式 A |
| Connection 每 worker 独立 | 非线程安全（绑一 Proactor） | N worker = N Connection |
| SyncState 加 fb2::Mutex | nlohmann 非线程安全 | state_ 加锁 |

---

## external/ 目录结构（Task 0 落地）

```
tdx-cpp/
├── external/                    # 第三方依赖统一收纳（.gitignore，不入 git）
│   ├── helio/                   # symlink → ~/framework/dragonfly/helio
│   │                             #   或 ln -s <helio-src> external/helio
│   └── duckdb/                  # vendored（从 libduckdb-linux-amd64.zip 解压，镜像 ghfast.top）
│       ├── include/duckdb.hpp
│       └── lib/libduckdb.so
├── .gitignore                   # 追加 external/
└── CMakeLists.txt               # HELIO_PATH 默认 external/helio；DuckDB IMPORTED；去 Arrow
```

**.gitignore 追加**：
```
external/
```

**CMakeLists.txt 变更**：
```cmake
# helio：默认 external/helio（symlink），保留 -DHELIO_PATH 可配
if(NOT HELIO_PATH)
  if(DEFINED ENV{HELIO_PATH}) set(HELIO_PATH $ENV{HELIO_PATH})
  else() set(HELIO_PATH "${CMAKE_SOURCE_DIR}/external/helio" CACHE PATH "" FORCE)
  endif()
endif()

# DuckDB：IMPORTED（vendored external/duckdb）
add_library(duckdb SHARED IMPORTED)
set_target_properties(duckdb PROPERTIES
  IMPORTED_LOCATION ${CMAKE_SOURCE_DIR}/external/duckdb/lib/libduckdb.so
  INTERFACE_INCLUDE_DIRECTORIES ${CMAKE_SOURCE_DIR}/external/duckdb/include)

# 删除：find_package(Arrow REQUIRED) / find_package(Parquet REQUIRED)
```

---

## Patterns to Mirror

**PATTERN 1：DuckDB 嵌入式（Parquet 读写 + 查询 + 内存表，替代 Arrow + dragonfly）**
```cpp
#include <duckdb.hpp>
duckdb::DuckDB db(nullptr);
duckdb::Connection con(db);

// ① 写 Parquet（替代 Arrow writer）
con.Query("CREATE TABLE bars(code VARCHAR, datetime BIGINT, open DOUBLE, high DOUBLE, "
          "low DOUBLE, close DOUBLE, volume DOUBLE, amount DOUBLE)");
con.Query("INSERT INTO bars VALUES ('600000', 1719000000, 10.0, 10.5, 9.8, 10.2, 100000, 1020000)");
con.Query("COPY bars TO '~/.tdx-cpp/parquet/sh/600000/1d.parquet' (FORMAT PARQUET)");

// ② 读 Parquet（替代 Arrow reader）
auto r = con.Query("SELECT * FROM '~/.tdx-cpp/parquet/sh/600000/1d.parquet' "
                   "WHERE datetime >= 1718000000 ORDER BY datetime");

// ③ 即席 SQL 查询（回测）
auto r2 = con.Query("SELECT code, MAX(high) FROM 'parquet/sh/*.parquet' GROUP BY code");

// ④ 内存热表（替代 dragonfly 热缓存——最新报价）
con.Query("CREATE MEMORY TABLE latest(code VARCHAR, price DOUBLE, ts BIGINT)");
con.Query("INSERT INTO latest VALUES ('600000', 10.5, 1719000000)");
```

**PATTERN 2：helio 并发池**（simple_channel.h + server_pool.cpp:45-60）
```cpp
util::fb2::SimpleChannel<Task> chan(N*2, 1);  // 背压
pb->Await([&] {
  std::vector<util::fb2::Fiber> workers;
  for (int i=0;i<N;++i) workers.push_back(util::MakeFiber([&,i]{
    tdx::proto::Connection conn(pb); conn.Connect(best);
    Task t; while (chan.Pop(t)) { /* fetch + duckdb parquet + sync */ }
  }));
  for (auto& c : codes) chan.Push({c});
  chan.StartClosing();
  for (auto& w : workers) w.Join();
});
```

**PATTERN 3：断点续传 schema**（SyncEntry 加 batch_id/batch_status/retry_count）

**PATTERN 4：fiber 纪律**（沿用）+ nlohmann json 加 fb2::Mutex + std::filesystem

---

## Files to Create / Update

| 文件 | 动作 | 说明 |
|---|---|---|
| `external/` | CREATE | 统一第三方目录（helio symlink + duckdb vendored）+ .gitignore |
| `CMakeLists.txt` | UPDATE | HELIO_PATH 默认 external/helio；DuckDB IMPORTED；**删 find_package(Arrow Parquet)** |
| `.gitignore` | UPDATE | 追加 external/ |
| `include/tdx/data/sync_state.hpp` + `src/data/sync_state.cpp` | UPDATE | SyncEntry 加 batch 字段 + fb2::Mutex + 批次 API + std::filesystem |
| `include/tdx/query/duckdb_query.hpp` + `src/query/duckdb_query.cpp` | CREATE | DuckDB 嵌入式：Parquet 读写 + SQL 查询 + 内存热表 |
| `src/query/CMakeLists.txt` | CREATE | tdx_query 链 duckdb IMPORTED |
| `include/tdx/batch/batch_fetch.hpp` + `src/batch/batch_fetch.cpp` | CREATE | N worker + SimpleChannel |
| `src/cli/main.cpp` | UPDATE | absl flags（-n/--resume/--parquet）+ batch-fetch/sql |
| `tests/test_sync4.cpp` + `test_duckdb.cpp` + `test_batch.cpp` | CREATE | 测试 |

---

## NOT Building

- **~~Arrow~~**（已修订去除）—— Parquet 读写全用 DuckDB
- **~~dragonfly / RESP 客户端~~**（v2 已去除）—— 用 DuckDB 内存表替代
- **HTTP 爬虫** / **Python 绑定** —— Won't

---

## Step-by-Step Tasks

### Task 0: external/ 统一 + 去除 Arrow（前置，项目级重构）
- **ACTION**:
  1. 建 `external/helio` symlink → `~/framework/dragonfly/helio`（或用户指定 helio 源）
  2. 下载 DuckDB libduckdb-linux-amd64.zip（镜像 ghfast.top）→ 解压到 `external/duckdb/`（include/ + lib/）
  3. `.gitignore` 追加 `external/`
  4. `CMakeLists.txt`：HELIO_PATH 默认改 `${CMAKE_SOURCE_DIR}/external/helio`；加 DuckDB IMPORTED target；**删 `find_package(Arrow Parquet REQUIRED)`** 及所有 Arrow/Parquet 引用
  5. 验证 Phase 1-3 仍能 build（helio 经 external/helio symlink）
- **GOTCHA**: helio symlink 需用户本地建（external/ 不入 git，文档说明 `ln -s`）；DuckDB lib 版本固定；去 Arrow 后 tdx_proto 无 Parquet 依赖（本就无）
- **VALIDATE**: Phase 1-3 全量 build + ctest 通过（helio 经 external/）

### Task 1: SyncState 增强（断点续传中枢）
- SyncEntry 加 batch_id/batch_status/retry_count/last_error；state_ 加 fb2::Mutex；StartBatch/IsCompletedInBatch/MarkStockComplete/MarkStockFailed；std::system → std::filesystem
- VALIDATE: 单元测试（批次/崩溃恢复/并发安全）

### Task 2: DuckDB 查询层（Parquet 读写 + SQL + 内存表，合并原 Task 2+5）
- **ACTION**: duckdb_query（QueryParquet SELECT FROM + WriteParquet COPY TO + 内存热表 SetLatest/GetLatest）；tdx_query 链 duckdb IMPORTED
- **GOTCHA**: DuckDB 内置 Parquet（无 Arrow）；内存表替代 dragonfly；文件 ~/.tdx-cpp/parquet/<market>/<code>/<period>.parquet
- **VALIDATE**: 单元测试（COPY TO 写 + SELECT FROM 读 + 内存表 + schema 一致）

### Task 3: helio 并发批量（-n）
- batch_fetch（N worker fiber + SimpleChannel，每 worker 独立 Connection）
- VALIDATE: 单元测试（5000 股 mock + -n 1/4/16 + 背压）

### Task 4: 断点续传集成
- FetchHistory 增量（start 生效）+ 跳过 IsCompletedInBatch + --resume 崩溃恢复
- VALIDATE: 集成测试（中断→重启→跳过已完成）

### Task 5: 存储分层集成
- TdxData/FetchHistory 集成 DuckDB（worker 完成 → COPY TO Parquet + 内存热表）+ 并发调度
- VALIDATE: 集成测试（并发抓取→Parquet→DuckDB 查询）

### Task 6: CLI 完整化
- absl flags（-n/--resume/--parquet）+ batch-fetch --stock-list + sql "SELECT ..."（DuckDB 即席查询）
- VALIDATE: CLI 烟雾测试

### Task 7: 测试 + 端到端验收
- test_sync4/duckdb/batch；端到端（batch-fetch 100 股 -n 8 --parquet --resume + DuckDB 查询）
- VALIDATE: 全套 + 并发性能（5000 股 -n 16 vs 串行）+ DuckDB 读 Parquet

---

## Acceptance Criteria

- [ ] ① 并发批量：5000 股 -n 16 显著快于串行（~10x）
- [ ] ② 股票级断点续传：中断重启跳过已完成
- [ ] ③ Parquet 落盘：DuckDB SELECT FROM parquet，schema 一致（**无 Arrow**）
- [ ] ④ DuckDB 内存热表：最新报价 INSERT/SELECT（替代 dragonfly）
- [ ] ⑤ 第三方依赖统一 external/（helio + duckdb），external/ 不入 git
- [ ] ⑥ Phase 1-3 全量 build + ctest 通过（helio 经 external/helio）
- [ ] ⑦ fiber 纪律无违规（fb2::Mutex + std::filesystem）

---

## Risks and Mitigations

| 风险 | 可能性 | 影响 | 缓解 |
|---|---|---|---|
| external/ helio symlink 缺失 | 中 | build 失败 | 文档说明 ln -s；HELIO_PATH fallback |
| DuckDB vendored lib ABI | 中 | 链接错 | 固定 release + 测试 |
| DuckDB lib 下载（镜像） | 中 | 获取失败 | ghfast.top + 校验 |
| 去 Arrow 后 Parquet schema 差异 | 中 | duckdb 读写不一致 | DuckDB 自洽（写读同引擎） |
| SimpleChannel 并发 | 中 | 死锁/OOM | 仿 server_pool + 单测 |
| nlohmann 并发 | 高 | 数据损坏 | fb2::Mutex |

---

## Notes

- **v3 修订核心**：① 去 Arrow（DuckDB 全覆盖 Parquet）；② external/ 统一（helio + duckdb）。
- **external/ 不入 git**（全局规范）：用户本地建 helio symlink + 解压 duckdb；README 文档化。
- **DuckDB 单引擎优势**：Parquet 读写 + SQL 查询 + 内存热表，一个依赖替代 Arrow + dragonfly。
- **实施顺序**：Task 0（external/ 重构，前置）→ 1（SyncState）→ 2（DuckDB）→ 3（并发）→ 4/5（集成）→ 6（CLI）→ 7（验收）。
- **版本号**：Phase 4 → 0.4.0。

---

*下一步：评审本计划 → 运行 `/prp-implement .claude/PRPs/plans/phase4-v2-improvements.plan.md` 执行 Phase 4。*
