# Feature: Phase 3 — 数据管理层（功能对齐 tdxdata）

## Summary

在 Phase 1（协议层）+ Phase 2（扩展行情+SP/MAC）基础上实现最上层数据管理：`TdxData` 统一 API（11 个 fetch_* 方法）、`DataManager` 三段式流水线（熔断→source→返回）、增量同步（SyncState JSON 持久化）、自算复权因子（Adjust，基于 xdxr）、A 股时段感知重采样（Resampler，15m/30m/1h←5m、1w/1mon←1d）、混合数据源（HybridSource，本地优先+网络补缺）、交易日历。完成后达成端到端功能对等：统一 API 可拉取+复权+重采样，覆盖 tdxdata 236 等价用例。

## User Story

**作为** 量化研究员，
**我想要** 一个统一 API 一次调用拿到「前复权 + 30 分钟周期 + 本地优先网络补缺」的 K 线，
**以便** 直接用于回测/因子计算，不必手动拼协议+复权+重采样。

## Problem Statement

tdxdata 把协议层（opentdx/mootdx）封装成统一 API，并叠加复权、时段重采样、增量同步、混合数据源。这些是数据语义层（非协议），C++ 移植需逐字对照算法（Adjust _per_share、Resampler bar_end_time、Hybrid 补缺策略），且 Calendar 委托 mootdx 网络爬虫（PRD Won't HTTP）需另寻数据源。

## Solution Statement

`tdx::data` 分层：Calendar（硬编码节假日）+ Adjust（xdxr 自算因子）+ Resampler（时段感知）+ SyncState（JSON）+ HybridSource（本地+网络）→ DataManager（三段式+增量）→ TdxData（11 fetch_*）。复用 Phase 1 `RetryPolicy`/`CircuitBreaker`（PRD 决策：对齐 opentdx，不实现 tdxdata 版）、`StdQuotes`/`VipdocReader`。简化 SourceRegistry 为具体类（评审 P2-2），不实现 Storage 层（v1 返回 vector）。

## Metadata

| Field | Value |
|---|---|
| Type | NEW_CAPABILITY（数据语义层从零搭建） |
| Complexity | HIGH（复权算法 + 时段重采样 + 混合数据源 + 增量同步，最抽象层） |
| Systems Affected | tdx::data（新层）、tdx::consts（时段）、CLI、tests |
| Dependencies | Phase 1/2 已就绪（StdQuotes/VipdocReader/Retry/CircuitBreaker/nlohmann/json） |
| Source PRD | `.claude/PRPs/prds/tdx-cpp.prd.md`（Phase 3） |
| 研究依据 | tdxdata api/data_manager/sync/adjust/base/hybrid_kline/calendar 逐字段映射 |
| Estimated Tasks | 12 |

---

## 关键移植风险（带证据，必须严格对齐）

| # | 风险 | 上游行为 | C++ 对齐 |
|---|---|---|---|
| R1 | `_per_share` 阈值 | `value/10 if value >= 1 else value`（adjust.py:151），"每10股"判定 `>=1` 非 `>0` | 严格 `>=1` |
| R2 | bar_end_time 上下午切分 | `t < 12*60` 切分支（base.py:68），用 12:00 非 11:30 | 严格对齐（11:30-12:00 算上午） |
| R3 | Hybrid 前后缺只补一段 | `_compute_remote_range` 第二个 if 覆盖第一个（hybrid_kline.py:78-95） | 前后同缺只补后缺 |
| R4 | qfq 基准日归一 | 所有因子除以最新累计因子（adjust.py:204-207），最新日因子=1 | qfq 末尾归一 |
| R5 | KLine 字段顺序 | `tdx::KLine` open/close/high/low（types.hpp）vs tdxdata open/high/low/close | 标准化输出时显式映射 |
| R6 | Calendar 数据源 | 委托 mootdx 网络爬虫（PRD Won't HTTP） | 硬编码 cfg/holidays.json |
| R7 | time.sleep | adjust.py:40 / errors.py:78 | `ThisFiber::SleepFor`（复用 Phase 1 Retry 已 fiber 安全） |

---

## Mandatory Reading

| 优先级 | 文件 | 为何读 |
|---|---|---|
| P0 | `/home/li/peiking88/tdxdata/tdxdata/api.py` | TdxData 11 方法签名 + 默认值 |
| P0 | `/home/li/peiking88/tdxdata/tdxdata/core/data_manager.py` | 三段式流水线 + 增量同步（取最早 last_sync） |
| P0 | `/home/li/peiking88/tdxdata/tdxdata/sources/adjust.py:49-214` | 复权因子算法（_per_share、qfq/hfq 方向、基准归一） |
| P0 | `/home/li/peiking88/tdxdata/tdxdata/sources/base.py:56-132` | bar_end_time_ashare + _resample_minute（CEIL 标签） |
| P0 | `/home/li/peiking88/tdxdata/tdxdata/sync.py` | SyncState JSON schema + GapDetector |
| P0 | `/home/li/peiking88/tdxdata/tdxdata/sources/hybrid_kline.py` | 本地优先+网络补缺（_compute_remote_range） |
| P1 | `/home/li/peiking88/tdxdata/tdxdata/calendar.py` | is_trading_day/get_trading_days（需替换底层） |
| P1 | Phase 1/2 已落地代码 | 复用范式（StdQuotes/VipdocReader/Retry） |

---

## Patterns to Mirror

**PATTERN 1：Adjust 因子公式**（adjust.py:67-98）
```cpp
// category in {1,2} 或 除权除息事件：
//   numerator   = pre_close - fenhong + peigujia * peigu
//   denominator = pre_close * (1 + songzhuangu + peigu)
//   qfq: event_factor = numerator / denominator
//   hfq: event_factor = denominator / numerator
// cumulative *= event_factor;  // qfq 从新到旧，hfq 从旧到新
// _per_share(v) = v/10 if v>=1 else v  ← R1 关键
```

**PATTERN 2：bar_end_time_ashare**（base.py:56-76）
```cpp
// 上午 9:30 开盘(570min)，下午 13:00(780min)；t<12*60(720) 切上下午分支（R2）
// elapsed = t - session_start；bar_end = session_start + (elapsed/period + 1)*period
```

**PATTERN 3：_resample_minute CEIL 标签**（base.py:101-132）
```cpp
// 源 5m 标签是 CEIL（周期结束时间）：09:35 = 09:30-09:34
// effective_start = label - source_minutes（5）；再用 bar_end_time_ashare 算目标标签
// groupby(label) 聚合：open=first/high=max/low=min/close=last/vol=sum/amount=sum
```

**PATTERN 4：SyncState JSON schema**（sync.py:47-59）
```json
{"600519": {"history_kline": {"last_sync": "2024-06-22", "updated_at": "iso8601"}}}
// 增量查询取所有股票最早 last_sync 作统一 start_date（粒度退化到批处理级）
```

**PATTERN 5：Hybrid 补缺**（hybrid_kline.py:62-162）
```cpp
// local 为空 → 全网络降级；否则 _compute_remote_range 算前缺/后缺（只补一段，R3）
// 合并：concat(local, remote).drop_duplicates(date, keep=last)；网络段不复权，合并后整体复权
```

**PATTERN 6：复用 Phase 1 弹性层**（PRD 决策）
```cpp
// DataManager 注入 proto::RetryPolicy + proto::CircuitBreaker（Phase 1，对齐 opentdx）
// 不实现 tdxdata 的 [1,2,4]s 版本
```

---

## Files to Create / Update

| 文件 | 动作 | 说明 |
|---|---|---|
| `include/tdx/consts.hpp` | UPDATE | 交易时段常量（kMorningOpen=9:30 等） |
| `cfg/holidays.json` | CREATE | A 股节假日表（硬编码，替代 mootdx 爬虫） |
| `include/tdx/data/calendar.hpp` + `src/data/calendar.cpp` | CREATE | IsTradingDay/GetTradingDays（读 holidays.json） |
| `include/tdx/data/adjust.hpp` + `src/data/adjust.cpp` | CREATE | ComputeFactorFromXdxr/ApplyAdjust（_per_share, qfq/hfq） |
| `include/tdx/data/resampler.hpp` + `src/data/resampler.cpp` | CREATE | BarEndTimeAShare/ResampleKline（15m/30m/1h←5m, 1w/1mon←1d） |
| `include/tdx/data/sync_state.hpp` + `src/data/sync_state.cpp` | CREATE | SyncState（JSON ~/.tdx-cpp/sync_state.json）+ GapDetector |
| `include/tdx/data/hybrid_source.hpp` + `src/data/hybrid_source.cpp` | CREATE | HybridSource（本地 VipdocReader + 网络 StdQuotes） |
| `include/tdx/data/sources.hpp` + `src/data/sources.cpp` | CREATE | 各 Source 适配（History/Minute/Local/Realtime/Tick/F10/Financial 包装 StdQuotes） |
| `include/tdx/data/data_manager.hpp` + `src/data/data_manager.cpp` | CREATE | DataManager（三段式 + 增量，复用 Retry/CircuitBreaker） |
| `include/tdx/data/tdx_data.hpp` + `src/data/tdx_data.cpp` | CREATE | TdxData（11 fetch_* 统一 API） |
| `include/tdx/data/config.hpp` + `src/data/config.cpp` | CREATE | Config（config.json 测速缓存，对齐 mootdx config.py） |
| `src/cli/main.cpp` | UPDATE | 加 fetch/sync/adjust/resample 子命令 |
| `tests/test_adjust.cpp` + `test_resampler.cpp` + `test_sync.cpp` + `test_hybrid.cpp` + `test_tdxdata.cpp` | CREATE | 单元测试 |
| `scripts/extract_tdxdata_cases.py` | CREATE | 从 tdxdata 测试提取 236 等价用例（移植对照） |

---

## NOT Building（Phase 3 范围外）

- **并发批量**（helio fiber 池 + -n）→ Phase 4
- **股票级断点续传**（每股票完成后更新进度）→ Phase 4（v1 仅批处理级增量）
- **落盘存储**（dragonfly 热缓存 + Parquet 冷存储）→ Phase 4（v1 返回 vector，无 Storage 层）
- **HTTP 爬虫**（Calendar mootdx 委托、同花顺复权、gitee 财务）→ Won't（Calendar 用硬编码 holidays）
- **SourceRegistry 虚函数插件** → 简化为具体类（评审 P2-2）
- **tdxdata 版 RetryPolicy**（[1,2,4]s）→ 复用 Phase 1 opentdx 版（PRD 决策）

---

## Step-by-Step Tasks

### 基础算法

#### Task 1: Calendar + 交易时段常量
- **ACTION**: consts.hpp 加时段常量（kMorningOpen=570min/kAfternoonOpen=780min 等）；新建 calendar.{hpp,cpp}（IsTradingDay/GetTradingDays，读 cfg/holidays.json）；生成 holidays.json（A 股近年节假日）
- **MIRROR**: tdxdata calendar.py:11-61（API）；base.py:62-63（时段）
- **GOTCHA**: mootdx holiday 是网络爬虫（Won't），用硬编码 holidays.json 替代；时段用分钟数（570/780）
- **VALIDATE**: 单元测试（节假日判定 + 区间枚举）

#### Task 2: Adjust 复权因子
- **ACTION**: adjust.{hpp,cpp}：ComputeFactorFromXdxr(xdxr, kline, qfq/hfq) + ApplyAdjust(kline, factor)；_per_share（>=1 判定 R1）
- **MIRROR**: adjust.py:49-214（compute_factor_from_xdxr + apply_adjust）
- **GOTCHA**: qfq 从新到旧遍历+backward-asof+末尾除以最新因子（R4）；hfq 从旧到新+forward-asof；仅 OHLC 乘因子，vol/amount 不变
- **VALIDATE**: 单元测试（送股/分红/配股事件因子 + qfq/hfq 对照手算）

#### Task 3: Resampler 时段感知重采样
- **ACTION**: resampler.{hpp,cpp}：BarEndTimeAShare(dt, period_min)（R2 t<720 切分支）+ ResampleKline(kline, target_freq)；_resample_minute（CEIL 标签 R3）+ 周月重采样
- **MIRROR**: base.py:56-132
- **GOTCHA**: 源 5m 标签是 CEIL（结束时间），effective_start = label - 5min；15m/30m/1h←5m，1w/1mon←1d；聚合 first/max/min/last/sum/sum
- **VALIDATE**: 单元测试（9:30 5m→15m 标签 10:00；跨午休；周月边界）

### 状态与数据源

#### Task 4: SyncState + GapDetector
- **ACTION**: sync_state.{hpp,cpp}：SyncState（Load/Save/GetLastSync/UpdateSync，JSON ~/.tdx-cpp/sync_state.json）+ GapDetector（detect 缺口，工作日 freq）
- **MIRROR**: tdxdata sync.py:16-114
- **GOTCHA**: schema {stock:{data_type:{last_sync, updated_at}}}；GapDetector 用工作日（不含节假日，与上游一致）；JSON 用 nlohmann
- **VALIDATE**: 单元测试（JSON 读写 + 缺口检测）

#### Task 5: HybridSource 混合数据源
- **ACTION**: hybrid_source.{hpp,cpp}：FetchHybrid(code, start, end, period, adjust) 本地优先（VipdocReader）+ 网络补缺（StdQuotes）；ComputeRemoteRange（前后缺只补一段 R3）
- **MIRROR**: hybrid_kline.py:62-162
- **GOTCHA**: 本地空→全网络；合并 drop_duplicates(date, keep=last)；网络段不复权，合并后整体复权；本地仅 1d/1m/5m
- **VALIDATE**: 单元测试（前缺/后缺/全本地/全网络）+ live（混合拉取）

#### Task 6: Sources 适配层
- **ACTION**: sources.{hpp,cpp}：包装 StdQuotes 的 HistoryKline/MinuteKline/LocalKline/Realtime/Tick/F10/Financial source（每个 fetch 方法）
- **MIRROR**: tdxdata sources/*（history_kline/minute_kline/local_kline/realtime_snapshot/tick/f10/financial）
- **GOTCHA**: HistoryKline 默认 qfq + 支持 RESAMPLE_MAP 递归；MinuteKline 不复权不重采样；F10 逐 section
- **VALIDATE**: 编译 + 各 source 烟雾测试

### 管理层

#### Task 7: DataManager 三段式流水线
- **ACTION**: data_manager.{hpp,cpp}：Fetch(source, kwargs) 三段式（熔断→source→返回）+ 增量同步（ApplyIncremental 取最早 last_sync）+ UpdateSyncState；复用 proto::RetryPolicy + proto::CircuitBreaker
- **MIRROR**: data_manager.py:52-140
- **GOTCHA**: 复用 Phase 1 Retry（不实现 tdxdata 版 PATTERN 6）；空结果早返；增量粒度退化到批处理级（严格对齐上游）
- **VALIDATE**: 单元测试（三段式流程 + 增量调整）

#### Task 8: TdxData 统一 API
- **ACTION**: tdx_data.{hpp,cpp}：TdxData 类（11 fetch_* 方法：fetch_history/realtime/kline/tick/f10/basic/financial/local/hybrid + sync_status + get_stock_name），懒连接，转发 DataManager
- **MIRROR**: tdxdata api.py:15-252
- **GOTCHA**: 默认值（fetch_history period=1d/dividend=front；fetch_local dividend=none）；_market_from_code 仅沪深（无北证 R 对齐）；上下文管理（Connect/Close）
- **VALIDATE**: 单元测试 + 端到端（fetch_history 前复权）

#### Task 9: Config 测速缓存
- **ACTION**: config.{hpp,cpp}：Config（读/写 config.json 测速缓存 ~/.tdx-cpp/config.json，对齐 mootdx config.py:15-52）
- **MIRROR**: mootdx config.py + server.py bestip
- **GOTCHA**: 保留 mootdx config.json 缓存机制（评审决策）；ServerPool 复用缓存
- **VALIDATE**: 单元测试（缓存读写）

### 集成

#### Task 10: CLI 完整化
- **ACTION**: cli/main.cpp 加子命令：`fetch-history <codes> <start> <end> [period] [dividend]`、`fetch-hybrid`、`adjust <code>`、`resample <code> <freq>`、`sync-status <code>`
- **MIRROR**: tdxdata CLI + mootdx __main__
- **GOTCHA**: 精度格式（价位 %.2f）；CLI 在 main 线程
- **VALIDATE**: CLI 烟雾测试（fetch-history 拉取+前复权）

#### Task 11: 测试 + tdxdata 等价用例
- **ACTION**: test_adjust/test_resampler/test_sync/test_hybrid/test_tdxdata；extract_tdxdata_cases.py 提取 236 等价用例对照
- **VALIDATE**: 全套测试通过 + 覆盖率 >80%

#### Task 12: 端到端验收
- **ACTION**: 端到端 live（fetch_history 前复权 + 30m 重采样 + 混合数据源）+ 验收对照
- **VALIDATE**: ① tdxdata 等价用例；② 统一 API 端到端；③ 覆盖率

---

## Testing Strategy

### 单元测试
| 测试 | 覆盖 |
|---|---|
| test_calendar | 节假日判定 + 区间枚举 |
| test_adjust | 送股/分红/配股因子 + qfq/hfq + _per_share |
| test_resampler | 5m→15m/30m/1h 标签 + 跨午休 + 周月 |
| test_sync | JSON 读写 + 缺口检测 |
| test_hybrid | 前缺/后缺/全本地/全网络 |
| test_data_manager | 三段式 + 增量 |
| test_tdxdata | 11 fetch_* 签名 + 默认值 |

### tdxdata 等价用例（Success Signal ①）
- extract_tdxdata_cases.py 提取 tdxdata 236 测试用例（172 单元 + 44 live + 20 local），C++ 对照

### live / local 测试
- local: VipdocReader .day → HybridSource 本地优先
- live: fetch_history 前复权 + fetch_hybrid 混合 + resample 30m

### Edge Cases
- [ ] _per_share >=1 判定（R1）
- [ ] bar_end_time 11:30-12:00 算上午（R2）
- [ ] Hybrid 前后同缺只补后缺（R3）
- [ ] qfq 末尾归一（R4）
- [ ] KLine 字段顺序映射（R5）

---

## Validation Commands

```bash
cmake --build build --target tdx test_adjust test_resampler test_sync test_hybrid test_data_manager test_tdxdata -j$(nproc)
ctest --test-dir build -R "^test_" -j$(nproc) --output-on-failure
./build/bin/tdx fetch-history 600000 2024-01-01 2024-06-01 1d front   # 前复权日K
./build/bin/tdx resample 600000 30m                                    # 30分钟重采样
./build/bin/tdx fetch-hybrid 600000 2024-01-01 2024-06-01             # 本地+网络混合
```

---

## Acceptance Criteria（对齐 PRD Phase 3 Success Signal）

- [ ] ① tdxdata 236 等价用例在 C++ 版通过（复权/重采样/缺口/混合）
- [ ] ② 统一 API 端到端可拉取 + 复权 + 重采样
- [ ] ③ 覆盖率 >80%
- [ ] ④ Calendar 节假日 + 时段常量
- [ ] ⑤ Adjust qfq/hfq 因子对照 tdxdata
- [ ] ⑥ Resampler 15m/30m/1h/1w/1mon 对照 tdxdata
- [ ] ⑦ HybridSource 本地+网络混合
- [ ] ⑧ fiber 纪律无违规（复用 Phase 1 Retry，无 time.sleep）

---

## Risks and Mitigations

| 风险 | 可能性 | 影响 | 缓解 |
|---|---|---|---|
| _per_share >=1 判定错 | 高 | 复权全错 | 严格对齐 adjust.py:151 + 单测 |
| bar_end_time 时段错 | 高 | 重采样标签错 | 严格 t<720 切分支 + 单测 |
| qfq 基准归一漏 | 中 | 前复权错 | 末尾除以最新因子（R4） |
| Hybrid 补缺策略错 | 中 | 数据缺口 | 严格只补一段（R3） |
| Calendar 节假日不全 | 中 | 误判交易日 | holidays.json 维护 + 文档 |
| KLine 字段顺序映射 | 中 | 输出错 | 标准化时显式映射（R5） |
| tdxdata 等价用例多（236） | 中 | 验收工作量大 | extract 脚本批量对照 |
| nlohmann JSON 在 fiber 内 | 低 | Proactor 阻塞 | JSON 操作挪 fiber 外（sync_state） |

---

## Notes

- **依赖关键路径**：Task 1-3（算法基础，可并行）→ Task 4-6（状态+数据源）→ Task 7-8（管理层）→ Task 9-10（配置+CLI）→ Task 11-12（测试+验收）。
- **复用 Phase 1/2**：RetryPolicy/CircuitBreaker（不复刻 tdxdata 版）、StdQuotes/ExtQuotes/VipdocReader、nlohmann/json、types.hpp（KLine/Quote/Tick/Xdxr）。
- **简化决策**：SourceRegistry→具体类（P2-2）；Storage→返回 vector（v1）；Calendar→硬编码 holidays。
- **版本号**：Phase 3 完成 → 0.3.0（增加功能升次版本号）。

---

*下一步：评审本计划 → 运行 `/prp-implement .claude/PRPs/plans/phase3-data-manager.plan.md` 执行 Phase 3。*
