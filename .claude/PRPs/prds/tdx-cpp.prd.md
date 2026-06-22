# tdx-cpp — 通达信行情数据 C++ 接口库

> 覆盖 opentdx（协议层）+ mootdx（行情接口层）+ tdxdata（数据管理层）全部功能，用 C++ 原生重写。

*Generated: 2026-06-22*
*Status: ARCHITECTURE REVIEWED — 架构评审通过；评审 P0/P1 反馈已采纳并修复（Retry/CircuitBreaker 前移 Phase 1、dragonfly 分层采纳、fiber 纪律入规范、测试策略补强）*

---

## Problem Statement

通达信（TDX）行情数据是 A 股量化研究的事实数据源，目前生态仅有 Python 实现（`opentdx`/`mootdx`/`tdxdata`）。这些实现存在三类成本：**(1) 部署摩擦**——量化生产环境（C++ 回测/实盘引擎）引入 Python 运行时 + pandas/numpy 依赖链过重；**(2) 协议知识碎片化**——通达信私有 TCP 二进制协议的细节散落在三个项目的 Python 源码里，无单一权威参考；**(3) 功能断层**——三层依赖（`tdxdata → mootdx → opentdx`）任一层变更都会向上传导风险，且 `tdxdata` 的并发/断点续传/存储能力存在已知缺陷。不解决的话，C++ 量化系统只能各自重新逆向协议、重复造轮子，且无法获得经过上游 332+236 个测试验证的功能对等性。

## Evidence

- **协议知识已验证**：前序分析逐行读取了 `opentdx/parser/baseParser.py`（请求封包）、`opentdx/client/baseStockClient.py:283-321`（收发+zlib解压）、`opentdx/utils/help.py:137-207`（变长价格/紧凑日期），协议细节明确可移植。
- **测试基线已存在**：opentdx 332 测试/90% 覆盖率；tdxdata 236 测试（172 单元 + 44 live + 20 local）；mootdx 含 freezegun 时间冻结测试。这些构成 C++ 版的功能对等验收基线。
- **上游缺陷已确认**（实读代码）：tdxdata 全市场抓取为纯串行 for 循环（`tdxdata/sources/base.py:184`），增量同步只到「天」不到「股票」（`tdxdata/sync.py:91`），存储后端除内存 DataFrame 桩外全部删除（`tdxdata/storage/dataframe.py:9`）。
- **异步框架已选型并验证**：helio（`~/framework/dragonfly/helio`）C++17、io_uring+epoll、自研 fiber 协程，`AddPeriodic` 定时器匹配 15s 心跳，`MakeFiber` 匹配并发测速，`examples/echo_server.cc` 提供现成 TCP 客户端模板。
- **假设**：C++ 版「被 C++ 量化系统实际采用」—— *需通过首个集成方验证*。

## Proposed Solution

构建**单一 C++ 库 `tdx-cpp`**，将三层 Python 实现合并为同源分层架构（协议层 → 行情接口层 → 数据管理层），外加 CLI 工具。库内数据采用强类型 C++ 结构体（`KLine`/`Quote`/`Tick` 等）+ `std::vector`，零外部数据框架依赖。**网络层采用 helio fiber 模型**（io_uring 优先），GBK 转码用系统 iconv，C++ 标准定为 C++17。**v1 严格追求与上游功能对等**，并发/断点续传/落盘存储等改进点推迟到 v2。

## Key Hypothesis

我们相信**提供一个功能与 opentdx+mootdx/tdxdata 对等、零 Python 依赖、基于 helio 高性能异步 IO 的 C++ 库**，将让 C++ 量化开发者**无需引入 Python 运行时即可获取通达信全量行情数据**。我们将在**首个 C++ 量化系统集成并跑通全量 live 测试**时确认这一假设成立。

## What We're NOT Building

- **Python 绑定（pybind11）** —— v1 不做。
- **并发批量下载 / 股票级断点续传** —— v1 不做。推迟到 v2（Phase 4）。
- **落盘存储后端** —— v1 不做。v1 数据仅在内存以结构体返回；持久化推迟到 v2（Phase 4，分层：dragonfly 热缓存 + Parquet/Arrow 冷存储）。
- **HTTP 爬虫类数据源**（同花顺复权因子、gitee 财务清单）—— v1 不做，Won't。
- **GUI / 交易下单** —— 完全不在范围。

## Success Metrics

| 指标 | 目标 | 度量方式 |
|---|---|---|
| 功能对等覆盖 | 上游三项目公开 API ≥ 95% 可用 | 逐 API 比对清单，标记 NotImpl |
| 协议测试对等 | 上游 332+236 测试的等价用例全部通过 | C++ 测试套件（含 live/local marker） |
| 协议正确性 | 二进制帧编解码与上游字节级一致 | 黄金字节流对比测试（详见 `phase1-golden-byte-stream.md`） |
| 代码覆盖率 | > 80%（遵循全局规范） | gcov/lcov，第三方协议库（helio/iconv）不计 |
| 部署 | 无 Python 依赖，helio 全家桶链接后体积可接受 | CMake 安装产物 + 二进制体积测量（注：helio 拉 Boost/abseil，非绝对轻量，相对 Python 运行时仍显著更轻） |

## Open Questions（全部 Resolved）

| # | 问题 | 决策 | 状态 |
|---|---|---|---|
| 1 | 异步框架？ | helio（io_uring+epoll+fiber） | ✅ Resolved |
| 2 | GBK 转码？ | 系统 iconv | ✅ Resolved |
| 3 | live 测试限流？ | 测试间加延迟/退避 | ✅ Resolved |
| 4 | config.json 测速缓存？ | 保留 | ✅ Resolved |
| 5 | C++ 标准？ | C++17 | ✅ Resolved |
| 6 | Retry/CircuitBreaker 归属 Phase？ | **前移 Phase 1**（评审 P0-1） | ✅ Resolved |
| 7 | dragonfly 存储？ | **取消硬性选型，采纳热缓存+冷存储分层**（评审 P1-2） | ✅ Resolved |
| 8 | fiber 纪律如何落实？ | **写入 CLAUDE.md 规范，按需应用**（评审 #2） | ✅ Resolved |
| 9 | 测试代码是否豁免 fiber 纪律？ | **豁免**（gtest 运行在 Proactor 线程外；被测 fiber 内代码仍须遵守）（评审 #3） | ✅ Resolved |
| 10 | Phase 1 黄金字节流覆盖哪些 msg_id？ | **制定细化方案单独评审**（见 `phase1-golden-byte-stream.md`） | 🔄 方案制定中 |

---

## Users & Context

**Primary User**
- **Who**：A 股量化交易/研究工程师，核心生产代码为 C++（回测引擎、因子计算、实盘信号）。
- **Current behavior**：要么调用 Python（mootdx/tdxdata）经 IPC/文件喂给 C++，要么自己逆向通达信协议。
- **Trigger**：需要在 C++ 进程内直接、低延迟、批量获取通达信历史 K 线 / 实时五档 / 逐笔 / 财务 / 除权除息。
- **Success state**：链接 `libtdx` + `#include <tdx/tdx.hpp>` 即可拉取数据，无需 Python。

**Job to Be Done**
当 C++ 量化系统需要通达信行情数据时，我想要一个原生 C++ 库直接提供对等功能，以便消除 Python 运行时依赖、降低部署复杂度、获得确定性性能。

**Non-Users**
- 仅用 Python 做研究的用户（继续用上游 Python 库即可）。
- 需要下单/交易的用户（本项目只读行情）。
- 需要图形界面看盘的用户。

---

## Solution Detail

### 核心能力（MoSCoW）

| 优先级 | 能力 | 来源 | 理由 |
|---|---|---|---|
| Must | TCP 连接 + 二进制帧编解码 + zlib + GBK(iconv) | opentdx | 协议层基石 |
| Must | 登录/心跳(15s, helio AddPeriodic)/自动重连/自动选服(helio MakeFiber 并发测速) | opentdx+helio | 连接稳定性 |
| Must | **RetryPolicy（指数退避）+ CircuitBreaker（CLOSED/OPEN/HALF_OPEN）** | opentdx+tdxdata | **协议层弹性，Phase 1**（评审 P0-1 前移；无此则 Phase 1 live 测试无熔断保护被限流卡死） |
| Must | 命令号→解析器注册表 | opentdx | 协议扩展机制 |
| Must | 变长价格 `get_price` + 紧凑日期 `to_datetime` 解码 | opentdx | 非平凡编码，易错 |
| Must | A 股标准行情：K线/分时/逐笔/五档/股票列表/数量/除权除息/财务/F10/指数 | opentdx+mootdx | 核心数据 |
| Must | 本地 vipdoc 文件读取（.day/.lc1/.lc5/block.dat） | opentdx+mootdx | 离线数据 |
| Must | Quotes 工厂分发（标准/扩展市场） | mootdx | API 组织 |
| Must | 复权因子自算（前复权 qfq / 后复权 hfq，基于 xdxr） | tdxdata | 量化必需 |
| Must | A 股交易时段感知 K 线重采样（15m/30m/1h←5m，1w/1mon←1d） | tdxdata | 数据正确性 |
| Must | 标准化输出 schema（stock_code/date/open/high/low/close/volume/amount） | tdxdata | 接口一致性 |
| Must | TdxData 统一 API（fetch_history/realtime/kline/tick/f10/financial/local/hybrid） | tdxdata | 用户入口 |
| Must | 增量同步状态（JSON 持久化）+ 数据缺口检测 | tdxdata | 增量拉取 |
| Must | 混合数据源（本地优先 + 网络补缺，双向） | tdxdata | 离线优先 |
| Must | 交易日历（is_trading_day/get_holidays/get_trading_days） | tdxdata | 日期处理 |
| Must | config.json 测速结果缓存（保留 mootdx 机制） | mootdx | 避免每次启动全量测速 |
| Must | CLI 工具（fetch/bars/quotes/server-test/validate） | 新增 | 运维与验证 |
| Should | 扩展行情（期货/港美股/期权/债券） | opentdx+mootdx | Phase 2 |
| Should | SP/MAC 高级行情（板块/资金流向/集合竞价/异动） | opentdx+mootdx | Phase 2 |
| Should | 财务数据下载解析（.dat/.zip，字段中文名 columns） | mootdx | 非协议核心 |
| Should | 最小可观测性（结构化日志：连接/熔断/心跳事件）+ 优雅关闭（drain fibers） | 新增（评审 P2-1） | 实盘可观测 |
| Could | 自动查找通达信安装目录 + connect.cfg | mootdx tdxfinder | 便利性 |
| Could | 插件注册表（source/storage）→ C++ 虚函数 | tdxdata | 扩展性（评审 P2-2 建议 v1 简化） |
| Won't | 并发批量下载 + 股票级断点续传 | tdxdata 短板 | **v1 不做，v2 Phase 4** |
| Won't | 落盘存储（dragonfly 热缓存 + Parquet/Arrow 冷存储分层） | tdxdata 短板 | **v1 不做，v2 Phase 4** |
| Won't | HTTP 爬虫（同花顺复权/gitee 财务清单） | mootdx contrib / opentdx crawler | 第三方站点不稳定 |
| Won't | Python 绑定 | — | 用户已排除 |

### MVP Scope

**Phase 1 交付物**即为 MVP：用 helio 连接通达信服务器，完成登录/心跳，在 **Retry/CircuitBreaker 弹性保护下**拉取 A 股 K 线、五档报价、分时、逐笔，并能读取本地 vipdoc 文件。

### User Flow（最短价值路径）

```
链接 libtdx → #include <tdx/tdx.hpp>
  → TdxData tdx;                          // helio ProactorPool 启动 + 自动选服(若 config.json 无缓存)
  → auto bars = tdx.fetch_history(        // 统一 API（受 CircuitBreaker 保护）
        {"600000"}, "2024-01-01", "2024-06-01", "1d", "qfq");
  → 遍历 std::vector<KLine> 直接使用      // 强类型结构体
```

---

## Technical Approach

**Feasibility**：**HIGH** —— 协议已被上游完整验证，helio 已验证匹配。主要工作量在二进制结构体逐字移植与变长编码逻辑。

### 架构总览（分层，对应三个上游 + helio 基础设施）

```
┌──────────────────────────────────────────────────────────────────┐
│  CLI 层                    tdx_cli 可执行文件                       │
│  子命令: fetch / bars / quotes / tick / server-test / validate     │
├──────────────────────────────────────────────────────────────────┤
│  数据管理层  tdx::data         [对齐 tdxdata]    ← Phase 3        │
│  TdxData / SyncState / GapDetector / HybridSource(本地优先+网络补缺)│
│  Adjust(复权自算) / Resampler(A股时段) / Calendar / SourceRegistry │
├──────────────────────────────────────────────────────────────────┤
│  行情接口层  tdx::quotes      [对齐 mootdx]      ← Phase 1/2      │
│  QuotesFactory(std/ext/sp) / StdQuotes / ExtQuotes / SPQuotes     │
│  Affair(财务下载) / FinancialReader / BlockReader / Config(缓存)  │
├──────────────────────────────────────────────────────────────────┤
│  协议层      tdx::proto       [对齐 opentdx]     ← Phase 1 核心   │
│  Connection(helio FiberSocket) / Frame(请求12B/响应16B)           │
│  Codec(zlib + iconv GBK) / Heartbeat(helio AddPeriodic 15s)       │
│  ServerPool(helio MakeFiber 并发测速)                              │
│  ★ RetryPolicy + CircuitBreaker（评审前移，协议层弹性）           │
│  ParserRegistry(msg_id→Parser) / 所有 *Parser 子类                │
│  VipdocReader(.day/.lc1/.lc5/.cfg) / BitMap(SP字段)               │
├──────────────────────────────────────────────────────────────────┤
│  基础设施    helio (util::fb2) + iconv + zlib                       │
│  ProactorPool(io_uring/epoll) / Fiber / FiberSocketBase            │
│  ⚠ fiber 纪律见 CLAUDE.md（禁用 std::mutex/sleep_for，用 fb2::）   │
└──────────────────────────────────────────────────────────────────┘
        v2 Phase 4 存储: dragonfly(热缓存,RESP) + Parquet/Arrow(冷)
```

**Retry/CircuitBreaker 归属协议层的理由**（评审 P0-1）：熔断器保护的是「连接通达信服务器」这一协议层行为，而非数据管理逻辑。Phase 1 的 live 测试和自动重连若没有熔断保护，通达信封 IP（已知中风险）会直接卡死 Phase 1 验收。它是协议层稳定性的必要组件，不是 tdxdata 数据管理能力。

**关键数据结构**（C++ 结构体，库内统一）：

```cpp
namespace tdx {
// 注：datetime 统一为 int64 epoch seconds（评审 P2-4，消除"epoch 或 YYYYMMDD" Dual Schema）
struct KLine  { std::string stock_code; int64_t datetime;
                double open, high, low, close; uint64_t volume; double amount; };
// 注：未填档位用 sentinel（如 NaN 或 -1）+ 文档约定，不用 0（评审 P2-3，避免被当有效报价）
struct Quote  { std::string stock_code; double price; uint64_t volume; double amount;
                std::array<double,5> bid, ask; std::array<int64_t,5> bid_vol, ask_vol; }; // 五档
struct Tick   { std::string stock_code; int64_t time; double price; uint64_t vol;
                int bid_no, ask_no; char flag; }; // 逐笔
struct Xdxr   { int64_t date; double send; double dividend; double rationed;
                double rationed_price; }; // 除权除息事件
}
```

### CMake target 结构

```
tdx-cpp/
├── CMakeLists.txt              project(tdx-cpp VERSION x.y.z) C++17
│                              find_package(Boost CONFIG REQUIRED context system)
│                              set(CMAKE_MODULE_PATH helio/cmake)
│                              add_subdirectory(third_party/helio)
├── third_party/helio/         helio 源码(git submodule/add_subdirectory)
├── src/
│   ├── util/       → tdx_util      (iconv GBK / zlib / log / time 封装)
│   ├── proto/      → tdx_proto     (协议层 + RetryPolicy + CircuitBreaker, link base fibers2 TRDP::uring)
│   ├── quotes/     → tdx_quotes    (行情接口层)
│   ├── data/       → tdx_data      (数据管理层)
│   └── cli/        → tdx_cli       (可执行文件)
├── include/tdx/                公共头文件（结构体、统一 API）
├── tests/                      单元(live/local marker)+ 集成 + fiber 时序测试
├── cfg/                        服务器列表、字段映射、市场代码常量、config.json 模板
├── docs/                       API 文档、协议说明、helio 集成指南
└── scripts/                    测速、批量、数据校验脚本
```

**helio 集成要点**：无 install/export target，**必须 `add_subdirectory` 内嵌**；系统须装 Boost（context+system），其余依赖（abseil/glog/liburing/xxhash/zstd/c-ares）由 helio 自动 FetchContent；关键 link target：`base`、`fibers2`、`TRDP::uring`。TCP 客户端模板 `helio/examples/echo_server.cc` 的 `Driver`/`TLocalClient`；dragonfly RESP 客户端模板 `dragonfly/src/server/protocol_client.h`（v2 存储用）。

### 协议层实现要点（移植自 opentdx，逐字对照）

| 关注点 | 上游位置 | C++ 实现要点 |
|---|---|---|
| 请求头 12B | `opentdx/parser/baseParser.py:9-25` | `pack("<BIBHH", head, customize, 1, lbody, lbody)`，head: 0x0c/0x1c |
| 响应头 16B | `opentdx/client/baseStockClient.py:299-308` | `unpack("<IBIBHHH")`，校验 prefix=`b1 cb 74 00` |
| zlib 解压 | `baseStockClient.py:310-319` | `zipsize != unzip_size` 时解压 body |
| GBK 解码 | `opentdx/parser/server.py:106` | 系统 iconv（GB18030/GBK→UTF-8） |
| 心跳 | `opentdx/utils/heartbeat.py:10-46` | helio `AddPeriodic(15000,...)`，20 次无业务断开，业务请求重置 |
| 自动选服 | `baseStockClient.py:138-177` | helio `MakeFiber` 并发测 TCP 延迟 + config.json 缓存 |
| RetryPolicy | `tdxdata/errors.py:42` | 指数退避 0.1/0.5/1/2s ×4，**Phase 1** |
| CircuitBreaker | `tdxdata/errors.py:90` | 5 次失败→OPEN，60s→HALF_OPEN，**Phase 1** |
| 变长价格 | `opentdx/utils/help.py:137-169` | bit0x40=符号，bit0x80=继续位，每后续 7bit |
| 紧凑日期 | `opentdx/utils/help.py:171-207` | 日K=YYYYMMDD；分钟线低16位=(year-2004)<<11\|mmdd，高16位=当日分钟 |
| K线请求体 | `opentdx/parser/quotation/kline.py:12` | `pack("<H6sHHHHH8s")`，code 是 6字节 GBK |

### helio 编码纪律

helio 的 Proactor 线程内禁用标准库阻塞原语（会阻塞整个事件循环）。**完整规范见 `CLAUDE.md`「helio fiber 编码纪律」节**——适用范围、测试代码豁免、第三方库审计、工程手段（CI lint 基线 + 头文件宏/static_assert 可选增强）。本 PRD 不重复，开发时遵循该规范。

### Technical Risks

| 风险 | 可能性 | 影响 | 缓解 |
|---|---|---|---|
| 变长价格/日期解码移植错误 | 高 | 数据价格/时间全错 | 黄金字节流单测（覆盖 msg_id 见 `phase1-golden-byte-stream.md`），对比上游逐字节 |
| helio fiber 纪律违反（误用 std::mutex 等） | 中 | Proactor 线程卡死 | CLAUDE.md 规范 + CI clang-tidy 扫描；测试代码豁免（运行在 Proactor 线程外）；fiber 内第三方库审计 |
| io_uring 内核要求（Linux 5.11+） | 中 | 旧内核回退 epoll 性能降 | 文档标注；CI 双测（5.11+ io_uring / 旧内核 epoll） |
| Boost 系统依赖缺失 | 低 | 构建失败 | README/CMake 明确前置依赖；提供安装脚本 |
| GBK 转码跨平台不一致 | 低 | 股票名乱码 | iconv 统一；测试覆盖中文股票名 |
| 通达信服务器限流/封 IP | 中 | live 测试不稳定 | **Retry/CircuitBreaker（Phase 1）保护** + live 测试间加延迟；mock 优先，live 可跳过 |
| SP 协议字段位图解析 | 中 | 板块/资金流失效 | 严格对照 `opentdx/utils/bitmap.py`，专项测试 |
| dragonfly 不适合磁盘落盘 | — | — | **已解决（评审 P1-2）**：取消硬性选型，采纳 dragonfly 热缓存 + Parquet 冷存储分层 |

---

## Implementation Phases

| # | Phase | 描述 | Status | Parallel | Depends | PRP Plan |
|---|---|---|---|---|---|---|
| 1 | 协议层 + A股标准行情核心 | helio 连接/帧/解析器注册表/编解码/心跳/选服/**Retry+CircuitBreaker** + StdQuotes + VipdocReader | in-progress | - | - | [plan](phase1-protocol-layer-a-share.plan.md) |
| 2 | 扩展市场 + SP高级行情 | exQuotationClient/macQuotationClient + ExtQuotes + SP协议 + 板块 | pending | 评估 | 1 | - |
| 3 | 数据管理层（功能对等） | TdxData统一API + Sync + Hybrid + 复权 + 时段重采样 + Calendar + config.json缓存 | pending | 评估 | 1 | - |
| 4 | 改进点（v2，已推迟） | 并发批量(-n) + 股票级断点续传 + 落盘存储(dragonfly热缓存+Parquet冷存储) | deferred | - | 3 | - |

### Phase 1: 协议层 + A股标准行情核心

- **Goal**：基于 helio 建立协议层骨架，在 Retry/CircuitBreaker 弹性保护下连真服拉 A 股核心数据。
- **Scope**：
  - `tdx::util`：helio ProactorPool 封装、iconv GBK、zlib、日志、时间、字节序工具（**全部遵守 CLAUDE.md fiber 纪律**）。
  - `tdx::proto`：`Connection`（封装 helio `FiberSocketBase`）、`Frame`、`Codec`、`Heartbeat`（`AddPeriodic(15000)`）、`ServerPool`（`MakeFiber` 并发测速 + config.json 缓存）、**`RetryPolicy`（指数退避 0.1/0.5/1/2s ×4）+ `CircuitBreaker`（5 次失败熔断 60s 半开）**、`ParserRegistry`、A股全部 `*Parser`（K线0x523/分时0x537/逐笔0xfc5/报价0x547/列表0x44d/数量0x44e/除权0x0f/财务0x10/F10/登录0x0d/心跳0x04）、`VipdocReader`（.day/.lc1/.lc5）。
  - `tdx::quotes::StdQuotes`：A股行情 API。
  - 结构体定义（`KLine`/`Quote`/`Tick`/`Xdxr`）。
  - 最小可观测性（结构化日志：连接/熔断/心跳事件）+ 优雅关闭（drain fibers）。
  - helio 集成：`add_subdirectory(third_party/helio)`、Boost 前置依赖。
- **Success signal**：① live 测试（间加延迟，受 CircuitBreaker 保护）连接真服拉到 600000 真实日K并与上游数据点一致；② local 测试解析真实 vipdoc `.day` 文件输出正确；③ **黄金字节流单测全过，覆盖 msg_id 详见 `phase1-golden-byte-stream.md`**（至少 0x523/0x547/0x0f/0xfc5/0x0d/0x04）；④ CLAUDE.md fiber 纪律审查无违规；⑤ RetryPolicy/CircuitBreaker 状态机测试通过；⑥ 覆盖率>80%。

### Phase 2: 扩展市场 + SP高级行情

- **Goal**：覆盖期货/港美股/期权/债券，以及板块/资金流/竞价/异动高级行情。
- **Scope**：`exQuotationClient`（端口 7727）+ 扩展 `*Parser`；`macQuotationClient`（SP 协议）+ SP `*Parser`（0x1218/0x122C/0x122E/0x1231/0x1237/0x123D）+ `BitMap`；`ExtQuotes`/`SPQuotes`；`BlockReader`（block.dat，2800B/块）。
- **Success signal**：① 扩展市场 live 测试拉到期货/港美股 K线；② SP 板块列表/资金流可获取；③ 与上游等价测试对齐。

### Phase 3: 数据管理层（功能对等）

- **Goal**：实现 tdxdata 的数据管理能力，达成端到端功能对等。
- **Scope**：`TdxData` 统一 API；`SyncState`/`GapDetector`（JSON 持久化 `~/.tdx-cpp/sync_state.json`）；`HybridSource`（本地优先+网络补缺，双向降级）；`Adjust`（基于 xdxr 自算前/后复权因子，对照 `tdxdata/sources/adjust.py:49`）；`Resampler`（A股时段感知，对照 `tdxdata/sources/base.py:56-132`）；`Calendar`；`Config`（保留 mootdx config.json 测速缓存）；`SourceRegistry`（虚函数插件，评审 P2-2 建议 v1 可简化为具体类）；CLI 工具完整化。
- **注**：RetryPolicy/CircuitBreaker 已在 Phase 1 实现，本 Phase 复用。
- **Success signal**：① tdxdata 236 个等价用例在 C++ 版全部通过；② 统一 API 端到端可拉取+复权+重采样；③ 覆盖率>80%。

### Phase 4: 改进点（v2，已推迟）

- **Goal**：补齐上游短板。
- **Scope**：
  - **并发批量**：helio fiber 池 + `-n` 参数控制并发。
  - **股票级断点续传**：每只股票完成后更新进度，崩溃可恢复。
  - **落盘存储（分层）**：
    - 热缓存：dragonfly（RESP 协议，用 helio 写客户端，参考 `dragonfly/src/server/protocol_client.h`）——最新报价、订阅状态、订单簿。
    - 冷存储：Parquet/Arrow 文件——历史 K线/逐笔，供回测。
    - *注：dragonfly 纯内存 Redis 兼容，KV 模型不适合 K线范围查询，故仅做热缓存；冷存储用列式文件。*
- **Status**：deferred，待 v1 功能对等验证后再启动。

### Parallelism Notes

- Phase 1 内部强串行（协议层是地基），不建议拆分并行。
- Phase 2 与 Phase 3 的**并行性待 Phase 1 验收后评估**（评审 P1-1 降级）：Retry/CircuitBreaker 已前移 Phase 1，Phase 3 的复权/重采样/Calendar/Sync/Hybrid 只依赖 Phase 1 的 K线/xdxr/报价数据，理论上可与 Phase 2 并行，但需确认公共头文件（`include/tdx/`）的分支合并策略。Phase 1 验收后再定。
- Phase 3 的 `Adjust`/`Resampler` 与 `HybridSource` 互相独立，可在 Phase 3 内并行实现。

---

## 需求功能点清单（从上游提取）

### A. 协议层（源自 opentdx）

| ID | 功能点 | 上游命令号/位置 |
|---|---|---|
| A01 | TCP 连接/断开/重连，IPv4/IPv6 自动（helio FiberSocket） | `baseStockClient.py:138-227` |
| A02 | 请求帧封包（12B 头 `<BIBHH` + body） | `parser/baseParser.py:9-25` |
| A03 | 响应帧解包（16B 头 `<IBIBHHH`） | `baseStockClient.py:299-308` |
| A04 | zlib 解压（zipsize≠unzip_size） | `baseStockClient.py:310-319` |
| A05 | GBK→UTF-8 中文解码（系统 iconv） | `parser/server.py:106` |
| A06 | 登录（msg 0x0d）+ 心跳（0x04，15s/20次，helio AddPeriodic） | `utils/heartbeat.py` |
| A07 | 自动选服（helio MakeFiber 并发测 TCP 延迟取最优 + config.json 缓存） | `baseStockClient.py:138-177` |
| A08 | 自动重试（指数退避 0.1/0.5/1/2s，4次） | `baseStockClient.py:78-87` |
| A09 | 命令号→Parser 注册表 | `parser/` 全部 `@register_parser` |
| A10 | 变长价格解码 `get_price` | `utils/help.py:137-169` |
| A11 | 紧凑日期解码 `to_datetime`（日K/分钟线） | `utils/help.py:171-207` |
| A12 | 价格/金额缩放（价/100，额*100，财务*10000） | `quotationClient.py:24-30` |
| A13 | A股 K线（0x523，请求体 `<H6sHHHHH8s`） | `parser/quotation/kline.py` |
| A14 | A股 分时图（0x537/0xfeb） | `parser/quotation/tick_chart.py` |
| A15 | A股 逐笔成交（0xfc5/0xfb5） | `parser/quotation/transaction.py` |
| A16 | A股 五档报价（0x547/0x53e/0x54b/0x54c） | `parser/quotation/quotes*.py` |
| A17 | A股 股票列表/数量（0x44d/0x44e） | `parser/quotation/list.py` |
| A18 | A股 除权除息（0x0f） | `parser/quotation/xdxr.py` |
| A19 | A股 财务（0x10） | `parser/quotation/finance.py` |
| A20 | A股 F10（0x2cf/0x2d0） | `parser/quotation/company_info*.py` |
| A21 | A股 指数行情/动量（0x51d/0x51c） | `parser/quotation/index*.py` |
| A22 | A股 成交量分布（0x51a） | `parser/quotation/volume_profile.py` |
| A23 | 扩展市场 商品列表/报价/K线/历史成交（0x23f4-0x2489） | `parser/ex_quotation/` |
| A24 | SP/MAC 板块/资金流/竞价/异动（0x1215-0x123E） | `parser/mac_quotation/` |
| A25 | SP 字段位图解析 | `utils/bitmap.py` |
| A26 | 本地 .day 读取（`<IffffIIf`） | `reader/daily_bar_reader.py:35` |
| A27 | 本地 .lc1/.lc5 读取（`<HHfffffII`/`<HHIIIIfII`） | `reader/min_bar_reader.py` |
| A28 | 本地 block.dat 板块（2800B/块） | `utils/block_reader.py:40-70` |
| A29 | 服务器列表（main/ex/broker/mac/mac_ex） | `const.py:4-193` |

### B. 行情接口层（源自 mootdx）

| ID | 功能点 | 上游位置 |
|---|---|---|
| B01 | Quotes.factory 工厂分发（std/ext） | `mootdx/quotes.py:26-43` |
| B02 | StdHqAdapter / ExtHqAdapter 签名适配 | `hq_adapter.py`/`exhq_adapter.py` |
| B03 | K线 bars(frequency/start/offset)，offset 上限 800 | `quotes.py:202-219` |
| B04 | 分笔 transaction 上限 2000 | `quotes.py:315-349` |
| B05 | K线周期常量（0=5m...11=年） | `consts.py:6-30` |
| B06 | 服务器测速（同步+异步）写 config.json（**保留**） | `server.py:42-71` |
| B07 | 财务文件下载解析（.dat/.zip + columns 中文名） | `affair.py`/`financial/` |
| B08 | 全局配置 settings + setup() 首次测速（**保留**） | `config.py:15-52` |
| B09 | 自动查找通达信安装目录 + connect.cfg | `tdxfinder.py` |
| B10 | retry 装饰器（tenacity 3 次随机退避） | `quotes.py:685-690` |
| B11 | 板块解析（委托 BlockReader） | `parse.py` |

### C. 数据管理层（源自 tdxdata）

| ID | 功能点 | 上游位置 |
|---|---|---|
| C01 | TdxData 统一入口 + fetch_* 方法 | `api.py:14` |
| C02 | fetch_history(含复权) | `api.py:47` |
| C03 | fetch_realtime 五档快照 | `api.py:68` |
| C04 | fetch_kline 盘中实时K线 | `api.py:83` |
| C05 | fetch_tick 逐笔（当日/历史） | `api.py:106` |
| C06 | fetch_f10（7 栏目） | `api.py:121` |
| C07 | fetch_basic 除权除息 | `api.py:130` |
| C08 | fetch_financial | `api.py:145` |
| C09 | fetch_local 本地二进制（1d/1m/5m） | `api.py:158` |
| C10 | fetch_hybrid 本地优先+网络补缺 | `api.py:179` |
| C11 | RetryPolicy（指数退避 1/2/4s，3 次）—— **Phase 1 实现，Phase 3 复用** | `errors.py:42` |
| C12 | CircuitBreaker（CLOSED/OPEN/HALF_OPEN，RLock）—— **Phase 1 实现，Phase 3 复用** | `errors.py:90` |
| C13 | SyncState JSON 持久化 + GapDetector | `sync.py:16,70,91` |
| C14 | 三段式 fetch 流水线（熔断→source→storage） | `core/data_manager.py:52-110` |
| C15 | 插件注册表（source/storage） | `core/registry.py` |
| C16 | 复权因子自算（qfq backward-asof / hfq forward-asof） | `sources/adjust.py:49` |
| C17 | A股时段感知重采样 bar_end_time_ashare | `sources/base.py:56-132` |
| C18 | FREQUENCY_MAP / RESAMPLE_MAP | `sources/base.py:15-41` |
| C19 | STANDARD_COLUMNS 标准化 schema | `sources/base.py:31` |
| C20 | Calendar is_trading_day/get_holidays/get_trading_days | `calendar.py` |
| C21 | get_stock_name 代码→名称（内存缓存） | `api.py:232` |
| C22 | 混合补缺算法（算 [min,max] 缺口段） | `sources/hybrid_kline.py:62-97` |
| C23 | 混合降级（本地缺→全网络/网络断→返本地） | `sources/hybrid_kline.py:118-150` |

---

## 测试用例（从上游提取，作为功能对等验收基线）

**测试策略**（遵循全局规范：覆盖率>80%，真实优先于 mock，>50 条改验样本/总数，markers 分 live/local）：

### 单元测试（对齐 opentdx 332 + tdxdata 172）

| 类别 | 关键用例 | 上游对照 |
|---|---|---|
| 帧编解码 | 给定 msg_id+payload→封包字节；给定响应字节→解析字段；prefix 校验失败抛异常 | opentdx parser 测试 |
| zlib 解压 | zipsize=unzip_size（不解压）/ ≠（解压）；损坏数据异常 | `baseStockClient` 测试 |
| iconv GBK | 中文股票名/公告解码；非法序列处理 | 新增 |
| 变长价格 | 正数/负数/多字节边界；对照 `get_price` 黄金输出 | `help.py:137` 测试 |
| 紧凑日期 | 日K(YYYYMMDD)/分钟线（跨年/跨月/午休）；边界 2004 年起点 | `help.py:171` 测试 |
| 周期常量 | freq 0~13 映射；offset>800 截断 | mootdx consts 测试 |
| 心跳状态机 | helio AddPeriodic 15s 触发；业务请求重置；20 次纯心跳断开 | `heartbeat.py` 测试 |
| RetryPolicy 状态机 | 退避序列 0.1/0.5/1/2s；成功即止；耗尽抛异常 | tdxdata `errors.py` 测试 |
| CircuitBreaker 状态机 | CLOSED→OPEN（5次失败）→HALF_OPEN（60s）→CLOSED/OPEN | tdxdata `errors.py` 测试 |
| 复权因子 | 给定 xdxr 事件流→前复权/后复权因子；分红/配股/送转组合 | tdxdata `adjust.py` 测试 |
| 时段重采样 | 5m→15m/30m/1h；1d→1w/1mon；跨交易日；午休不合并 | tdxdata `base.py` 测试 |
| 缺口检测 | 本地[min,max]与请求区间的关系（前缺/后缺/全缺/无缺） | tdxdata `hybrid_kline.py` 测试 |
| 交易日历 | 节假日过滤；区间枚举 | tdxdata `calendar.py` 测试 |
| config.json 缓存 | 首次测速落盘；后续启动直读；缓存失效重测 | mootdx `config.py` 测试 |
| 标准化 | 各 source 输出统一列 schema | tdxdata `base.py` 测试 |

### helio fiber 异步时序测试（评审 P1-3 补充）

| 类别 | 关键用例 | 说明 |
|---|---|---|
| fiber 异常传播 | fiber 内 throw 未捕获 → 验证行为（崩 ProactorPool 还是单 fiber 死） | fiber 边界异常是 bug 高发区 |
| 心跳/选服时序 | AddPeriodic 20 次无业务触发断连；选服并发完成时间 | 明确时间快进手段（mock clock 或 helio TestProactor） |
| config.json 并发写 | 多 fiber 同时写缓存 → 验证串行化无损坏 | 选服 MakeFiber 竞态 |

### local 测试（对齐 tdxdata 20 + opentdx reader 测试）

- 解析真实 `.day` 文件（沪深各一只）→ 校验 OHLCV 与通达信一致。
- 解析 `.lc1`/`.lc5` → 校验时间戳与价格。
- 解析 `block.dat` → 校验板块成员。
- vipdoc 时效性/完整性校验。

### live 测试（对齐 tdxdata 44 + opentdx live，连真服，可跳过）

- **测试间适当增加延迟/退避**（决策 #3），且**全程受 CircuitBreaker 保护**。
- 连接 → 登录 → 拉 600000 日K → 校验最新一根数据合理。
- 五档报价非空、价格>0；未填档位符合 sentinel 约定。
- 逐笔成交时间递增；股票列表数量 > 4000；除权除息事件非空。
- 扩展市场（Phase 2）：期货 K线、港美股报价；SP：板块列表、资金流。
- **live 返回>50 条时验证样本/总数**（遵循全局规范）。

### 集成测试

- 端到端：`TdxData.fetch_history({"600000","000001"}, ..., "qfq")` 返回复权后 K线。
- 混合源：本地有部分数据 + 网络补缺 → 合并结果连续无缺口。
- CLI：`tdx fetch history ...` 输出与库 API 一致。

---

## Decisions Log

| 决策 | 选择 | 备选 | 理由 |
|---|---|---|---|
| 语言/形态 | C++ 库 + CLI | Python 绑定 / 纯 CLI | 用户指定 |
| 数据格式 | C++ 结构体 + std::vector | Arrow / 结构体+导出层 | 用户指定 |
| v1 范围 | 三阶段递进 | 最小可用 / 全覆盖 | 用户指定 |
| 改进优先级 | 严格功能对齐优先 | 并发/存储优先 | 用户指定 |
| 异步 IO | helio（io_uring+epoll+fiber） | asio / 自写 | 用户指定 |
| C++ 标准 | C++17 | C++20 | 用户指定（helio 强制 C++17） |
| GBK 转码 | 系统 iconv | 内置码表 / ICU | 用户指定 |
| live 测试 | 间加延迟/退避 | 无间隔 | 用户指定 |
| 测速缓存 | 保留 mootdx config.json | 每次全量测速 | 用户指定 |
| **Retry/CircuitBreaker 归属** | **Phase 1（协议层）** | Phase 3（数据管理层） | **评审 P0-1**：熔断器保护连接，是协议层稳定性组件 |
| **v2 存储** | **dragonfly 热缓存 + Parquet 冷存储分层** | 纯 SQLite / 纯 dragonfly | **评审 P1-2**：取消 dragonfly 硬性选型（KV 不适合 K线范围查询），采纳分层 |
| **fiber 纪律落实** | **写入 CLAUDE.md 规范，按需应用** | 强制头文件宏拦截 | **评审 #2**：规范为主，CI lint + 宏拦截为可选增强 |
| **测试代码豁免 fiber 纪律** | **豁免**（边界：被测 fiber 内代码须遵守；编排用 Await/Join） | 全库强制 | **评审 #3**：gtest 运行在 Proactor 线程外 |
| Phase 2/3 并行 | 待 Phase 1 验收后评估 | 直接并行 | 评审 P1-1：前移 Retry 后前提变化 |
| 黄金字节流 msg_id | 细化方案单独评审 | 写进 PRD | 评审 #5 |
| 并发/断点/存储 | Won't（v1）→ Phase 4（v2） | v1 即做 | 用户 Q4 选择 |

---

## Research Summary

**Market Context**
- 通达信协议 C++ 实现极少且分散；pytdx/衍生（opentdx/mootdx）是 Python 事实标准。
- C++ 量化系统普遍自研协议层或经 IPC 调 Python，存在重复劳动。
- tdxdata 的插件/熔断/增量/混合源设计是上游最成熟工程实践，值得 C++ 版保留（功能对等）。

**Technical Context**
- 协议可行性 HIGH：上游完整实现且高测试覆盖，C++ 是确定性翻译。
- **helio 已验证匹配**：C++17、io_uring+epoll、自研 fiber，`AddPeriodic`/`MakeFiber`/`FiberSocketBase` 覆盖心跳/测速/TCP；纪律（禁用 std::mutex）写入 CLAUDE.md；需系统 Boost，无 install target 须 add_subdirectory。
- **dragonfly 是 Redis 兼容纯内存存储**：评审已定取消硬性选型，v2 仅做热缓存，冷存储用 Parquet/Arrow；可用 helio 写 RESP 客户端（参考 `dragonfly/src/server/protocol_client.h`）。
- 最大风险在非平凡编码（变长价格/紧凑日期）、helio fiber 纪律、SP 字段位图，需专项测试与 CI 扫描。
- 上游三大短板（串行/断点到天/存储桩）在 v1 不修，作为 v2 改进 backlog。

---

*评审历史：`/home/li/peiking88/tdx-cpp/.claude/PRPs/reviews/tdx-cpp.architecture-review.md`（REQUEST CHANGES → 已按用户 5 条决策修复 P0/P1）*
*下一步：评审黄金字节流方案 `phase1-golden-byte-stream.md` → 运行 `/prp-plan` 生成 Phase 1 实施计划。*
