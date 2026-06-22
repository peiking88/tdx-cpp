# Feature: Phase 4 — v2 改进（并发批量 + 股票级断点续传 + 落盘存储分层）

## Summary

补齐上游三短板：(1) **并发批量下载**（helio fiber 池 + `-n` 控并发，替代 tdxdata 纯串行 `base.py:184`）；(2) **股票级断点续传**（每股票完成即更新进度，崩溃可恢复，优于上游"只到天" `sync.py:91`）；(3) **落盘存储分层**——dragonfly 热缓存（最新报价/订阅，自写 RESP 客户端）+ Parquet 冷存储（历史 K线/逐笔，系统 Arrow 23.0.1）。完成后支持 5000 股票并发批量抓取（-n 16）+ 崩溃恢复 + Parquet 落盘供回测。

## User Story

**作为** 全市场量化研究员，
**我想要** 一次命令并发抓取 5000 股票日 K 并落盘 Parquet，崩溃后从断点续传，
**以便** 高效构建回测数据集，不必担心中断重来。

## Problem Statement

tdxdata 三个已知短板：① 全市场抓取纯串行（数小时级）；② 增量同步只到天不到股票（崩溃丢进度）；③ 存储全是桩（内存返回）。C++ v2 用 helio fiber 池 + SyncState 批次 schema + dragonfly/Parquet 分层解决。

## Solution Statement

`tdx::data::sync`（批次 schema + fb2::Mutex）+ `tdx::batch`（helio SimpleChannel worker 池）+ `tdx::storage`（Arrow Parquet writer/reader）+ `tdx::cache`（RESP 客户端连 dragonfly）。CLI `batch-fetch -n N --resume <batch_id> --parquet --cache`。复用 Phase 1-3 全部（Connection/Retry/CircuitBreaker/StdQuotes/TdxData）。

## Metadata

| Field | Value |
|---|---|
| Type | ENHANCEMENT（v2 改进） |
| Complexity | HIGH（并发 fiber 池 + 断点续传状态机 + Arrow Parquet + RESP 客户端，四方向交织） |
| Systems Affected | tdx::data::sync（增强）、tdx::batch（新）、tdx::storage（新）、tdx::cache（新）、CLI、tests |
| Dependencies | 系统已装 Arrow 23.0.1 + Parquet 23.0.1（apt）；helio（内嵌）；dragonfly facade::RedisParser（内嵌，不复用 ProtocolClient） |
| Source PRD | `.claude/PRPs/prds/tdx-cpp.prd.md`（Phase 4） |
| Estimated Tasks | 8 |

---

## 关键技术决策（带证据）

| 决策 | 依据 | 影响 |
|---|---|---|
| 不复用 dragonfly ProtocolClient | 它拖整套 server（protocol_client.cc:23-33 依赖 main_service/journal/rdb_load） | 自写极薄 RESP 客户端（FiberSocket + facade::RedisParser） |
| RESP 用 multi-bulk 格式 | inline 格式 value 含空格会错 | BuildResp({"SET",k,v}) multi-bulk |
| 并发用 N worker + SimpleChannel | helio 无 Semaphore/RateLimiter；SimpleChannel 满则背压（simple_channel.h:97） | 模式 A（非 5000 fiber） |
| Connection 每 worker 独立 | Connection 非线程安全（connection.hpp:29 绑一 Proactor） | N worker = N Connection |
| Parquet 用 Arrow C++ 23.0.1 | 系统 apt 已装（pkg-config arrow=23.0.1） | find_package(Arrow Parquet) |
| SyncState 加 fb2::Mutex | nlohmann::json 非线程安全，多 fiber 写崩 | state_ 加锁 |
| 删 std::system("mkdir -p") | fork+exec 阻塞 Proactor（fiber 纪律违规） | 改 std::filesystem |

---

## Patterns to Mirror

**PATTERN 1：RESP 客户端**（dfly_bench.cc:1327-1359）
```cpp
// multi-bulk：*N\r\n$len\r\n<part>\r\n...
std::string BuildResp(std::initializer_list<std::string_view> parts);
// SET/GET/HSET/ZADD → sock_->Write(io::Buffer(cmd)) → RedisParser.Parse → RespVec
facade::RedisParser parser{facade::RedisParser::CLIENT, 1024};
```

**PATTERN 2：helio 并发池**（simple_channel.h + server_pool.cpp:45-60）
```cpp
util::fb2::SimpleChannel<Task> chan(N*2, 1);  // 背压
pb->Await([&] {
  std::vector<util::fb2::Fiber> workers;
  for (int i=0;i<N;++i) workers.push_back(util::MakeFiber([&,i]{
    tdx::proto::Connection conn(pb);  // 每 worker 独立连接
    conn.Connect(best);
    Task t; while (chan.Pop(t)) { /* fetch + parquet + sync */ }
  }));
  for (auto& c : codes) chan.Push({c});  // 满则背压
  chan.StartClosing();
  for (auto& w : workers) w.Join();
});
```

**PATTERN 3：Parquet 写**（arrow/builder + parquet/arrow/writer）
```cpp
// KLine 9 字段 → arrow::Table → parquet::arrow::WriteTable（SNAPPY 压缩）
auto schema = arrow::schema({field("datetime",timestamp(SECOND,"Asia/Shanghai")), field("open",float64()), ...});
// Builder.Append × N → Finish → Table::Make → WriteTable(path, row_group=128K)
```

**PATTERN 4：断点续传 schema**（sync_state.hpp 增强）
```json
{"600519":{"history_kline":{"last_sync":"2024-06-22","batch_id":"20260623T103000",
  "batch_status":"completed","retry_count":0}}}
```

**PATTERN 5：fiber 纪律**（沿用）+ nlohmann json 加 fb2::Mutex + std::filesystem

---

## Files to Create / Update

| 文件 | 动作 | 说明 |
|---|---|---|
| `include/tdx/data/sync_state.hpp` + `src/data/sync_state.cpp` | UPDATE | SyncEntry 加 batch_id/batch_status/retry_count/last_error + fb2::Mutex + StartBatch/IsCompletedInBatch/MarkComplete/MarkFailed + std::filesystem |
| `include/tdx/data/storage/parquet_writer.hpp` + `.cpp` | CREATE | KLine→Parquet（Arrow schema + WriteTable SNAPPY） |
| `include/tdx/data/storage/parquet_reader.hpp` + `.cpp` | CREATE | 读末行 datetime 做增量基准 |
| `src/data/storage/CMakeLists.txt` | CREATE | tdx_storage 链 Parquet::parquet_shared |
| `CMakeLists.txt` | UPDATE | find_package(Arrow Parquet REQUIRED) |
| `include/tdx/cache/resp_client.hpp` + `src/cache/resp_client.cpp` | CREATE | RESP 客户端（FiberSocket + RedisParser，SET/GET/HSET/ZADD） |
| `include/tdx/batch/batch_fetch.hpp` + `src/batch/batch_fetch.cpp` | CREATE | N worker + SimpleChannel 并发批量 |
| `src/cli/main.cpp` | UPDATE | absl flags（-n/--resume/--parquet/--cache）+ batch-fetch 子命令 |
| `tests/test_sync4.cpp` + `test_parquet.cpp` + `test_resp.cpp` + `test_batch.cpp` | CREATE | 测试 |

---

## NOT Building

- **复用 dragonfly ProtocolClient**（拖 server）—— 自写 RESP
- **dragonfly 做 K线主存储**（KV 不适合范围查询）—— 仅热缓存
- **HTTP 爬虫** / **Python 绑定** —— Won't

---

## Step-by-Step Tasks

### Task 1: SyncState 增强（断点续传中枢）
- **ACTION**: SyncEntry 加 batch_id/batch_status/retry_count/last_error；state_ 加 fb2::Mutex；新增 StartBatch/IsCompletedInBatch/MarkStockComplete/MarkStockFailed；std::system → std::filesystem
- **GOTCHA**: nlohmann 非线程安全（fb2::Mutex）；向后兼容（旧 schema 缺字段默认值）；mkdir 用 create_directories
- **VALIDATE**: 单元测试（批次启动/完成/崩溃恢复跳过 + 并发安全）

### Task 2: Parquet 冷存储
- **ACTION**: parquet_writer（KLine→Arrow Table→Parquet SNAPPY）+ parquet_reader（读末行 datetime）；CMake find_package(Arrow Parquet) + tdx_storage
- **GOTCHA**: KLine schema 9 字段（datetime timestamp + 6 float64 + 2 int32）；文件布局 ~/.tdx-cpp/parquet/<market>/<code>/<period>.parquet；增量 read-modify-write
- **VALIDATE**: 单元测试（写 1000 行 + 读回 + schema 一致）

### Task 3: helio 并发批量（-n）
- **ACTION**: batch_fetch（N worker fiber + SimpleChannel，每 worker 独立 Connection）；复用 ServerPool.SelectBest + RetryPolicy + CircuitBreaker
- **GOTCHA**: Connection 非线程安全（N worker=N Connection）；-n=worker 数；背压（SimpleChannel 满挂起）；fiber 内禁 LOG(FATAL)/CHECK
- **VALIDATE**: 单元测试（5000 股 mock + -n 1/4/16 耗时对比 + 背压不 OOM）

### Task 4: 断点续传集成
- **ACTION**: FetchHistory 增量（start 生效）+ 跳过 IsCompletedInBatch + MarkStockComplete/Failed；崩溃恢复（--resume <batch_id>）
- **GOTCHA**: 批次 ID（启动时间戳）；retry_count 达上限熔断；MarkComplete 即 Save
- **VALIDATE**: 集成测试（中断 → 重启 → 跳过已完成）

### Task 5: dragonfly RESP 热缓存
- **ACTION**: resp_client（FiberSocket + facade::RedisParser，BuildResp multi-bulk，Set/Get/HSet/ZAdd + ReadReply）
- **GOTCHA**: 不复用 ProtocolClient（拖 server）；multi-bulk 格式；缓存 API（SetLatestQuote/GetSubscription，JSON value）
- **VALIDATE**: 单元测试（连本地 dragonfly 6379，SET/GET/HSET round-trip）

### Task 6: 存储分层集成
- **ACTION**: TdxData/FetchHistory 集成 Parquet 写（worker 完成调 WriteKlineParquet）+ dragonfly 缓存（最新报价）+ 并发批量（batch_fetch 调度）
- **VALIDATE**: 集成测试（并发抓取 → Parquet 落盘 + dragonfly 缓存）

### Task 7: CLI 完整化
- **ACTION**: absl flags（-n/--resume/--parquet/--cache）+ batch-fetch 子命令（tdx batch-fetch --stock-list <file> -n 16 --resume <id> --parquet）
- **GOTCHA**: 接 absl flags（Phase 4 必须）；--stock-list 文件（每行一 code）
- **VALIDATE**: CLI 烟雾测试（batch-fetch 10 股 -n 4 --parquet）

### Task 8: 测试 + 端到端验收
- **ACTION**: test_sync4（批次+断点续传）+ test_parquet + test_resp + test_batch；端到端（batch-fetch 100 股 -n 8 --parquet --resume + 崩溃恢复）
- **VALIDATE**: 全套测试 + 并发性能（5000 股 -n 16 vs 串行）+ Parquet duckdb 读取

---

## Acceptance Criteria

- [ ] ① 并发批量：5000 股 -n 16 显著快于串行（~10x）
- [ ] ② 股票级断点续传：中断重启跳过已完成
- [ ] ③ Parquet 落盘：duckdb/pandas 可读，schema 与 KLine 一致
- [ ] ④ dragonfly 热缓存：最新报价可 SET/GET
- [ ] ⑤ fiber 纪律无违规（fb2::Mutex + std::filesystem）
- [ ] ⑥ 全套测试通过 + 覆盖率>80%

---

## Risks and Mitigations

| 风险 | 可能性 | 影响 | 缓解 |
|---|---|---|---|
| SimpleChannel 并发模型错 | 中 | 死锁/OOM | 仿 server_pool + 单元测试背压 |
| Arrow Parquet API 版本差异 | 低 | 编译错 | 系统 23.0.1 固定 |
| nlohmann 并发崩溃 | 高 | 数据损坏 | fb2::Mutex（必须） |
| 断点续传 race | 中 | 重复/漏抓 | MarkComplete 原子 + retry 上限 |
| Parquet 增量重写慢 | 中 | IO 瓶颈 | row_group 追加或批次级合并 |

---

## Notes

- **实施顺序**：Task 1（SyncState 中枢）→ Task 3（并发池）→ Task 2（Parquet）→ Task 5（dragonfly）→ Task 4/6（集成）→ Task 7（CLI）→ Task 8（验收）。
- **复用 Phase 1-3**：Connection/Retry/CircuitBreaker/ServerPool/StdQuotes/TdxData/SyncState。
- **版本号**：Phase 4 → 0.4.0。
- **状态**：v2 改进，PRD 标 deferred；本计划制定后可启动。

---

*下一步：评审本计划 → 运行 `/prp-implement .claude/PRPs/plans/phase4-v2-improvements.plan.md` 执行 Phase 4。*
