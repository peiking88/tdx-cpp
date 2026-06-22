# Feature: Phase 2 — 扩展行情（期货/港美股/期权）+ SP/MAC 高级行情（板块/资金流）

## Summary

在 Phase 1 协议层基础上新增两条独立协议链：**扩展行情**（exQuotationClient，端口 7727，head=1，覆盖期货/港美股/期权/债券的 K线/报价/列表/历史成交）与 **SP/MAC 高级行情**（mac_hosts，端口 7709，覆盖板块列表/成员报价/K线/异动/资金流/集合竞价，含 20 字节位图动态字段机制）。提供 `ExtQuotes` 与 `SPQuotes` 两个语义化 API + CLI 子命令。完成后能拉取期货/港美股 K 线、获取 A 股板块列表与资金流向。

## User Story

**作为** A 股 + 全球资产量化工程师，
**我想要** tdx-cpp 覆盖期货/港美股/期权行情，以及板块/资金流/异动高级行情，
**以便** 在 C++ 单库内完成全资产 + 筹码面分析，无需 Python 运行时。

## Problem Statement

opentdx 的扩展行情与 SP/MAC 协议各有 ~20/~14 个 parser，含两类非平凡机制：扩展行情的 `unpack_futures`（291+code_len 字节多段报价）与 SP 的位图动态字段（20 字节 160 位字段映射）。C++ 移植需逐字对照，且 CLAUDE.md 对这两套协议的 head/customize/登录描述有误，必须以源码为准纠正。

## Solution Statement

两套独立 parser 分区（`tdx::proto::ex` + `tdx::proto::sp`），复用 Phase 1 的 frame/codec/connection/重试/熔断基础设施；扩展行情用独立 `ExtConnection`（端口 7727，head=1，不发心跳，80B hex 登录），SP 复用标准 Connection + 切 mac_hosts；新增位图机制（`bitmap.hpp`）与 JSON 响应解析（SP 0x1218 资金流/所属板块）。

## Metadata

| Field | Value |
|---|---|
| Type | NEW_CAPABILITY |
| Complexity | HIGH（两套协议 + 位图 + JSON + ~34 parser，比 Phase 1 更大） |
| Systems Affected | tdx::proto（ex/sp 分区）、tdx::quotes（ExtQuotes/SPQuotes）、CLI、tests |
| Dependencies | Phase 1 已就绪（frame/codec/connection/retry/heartbeat/server_pool）；新增 nlohmann/json（vendored single-header，或手写最小 JSON） |
| Source PRD | `.claude/PRPs/prds/tdx-cpp.prd.md`（Phase 2） |
| 研究依据 | ex_quotation/ 20 parser + mac_quotation/ 14 parser + bitmap.py + block_reader.py 逐字段映射（见研究记录） |
| Estimated Tasks | 14 |

---

## 关键修正（CLAUDE.md 描述与源码不符，移植必须以源码为准）

| 项 | CLAUDE.md 描述 | 源码实际（证据） |
|---|---|---|
| 扩展行情 head/customize | customize=1 | **head=1, customize=0**（baseParser.py:25 第 2 参数是 head；所有 ex parser `@register_parser(0xXXXX, 1)`） |
| 扩展行情登录 | msg_id 0x2454（隐含 `<B=1`） | msg_id 0x2454，但**请求体是 80 字节固定 hex**（ex_quotation/server.py:7-19，逐字节硬编码） |
| 扩展行情心跳 | 全局 15s | **exQuotationClient 不发心跳**（未覆盖 doHeartBeat，baseStockClient 默认 pass） |
| SP head/customize | customize=1 或 2 | **head=0x0c, customize=0**（与标准一致，靠 msg_id 0x1215-0x123E + 请求体内嵌 ASCII 串区分） |
| SP 登录 | msg_id 0x2454 | **标准 msg_id 0x0d**（macQuotationClient 继承 QuotationClient.Login） |
| SP code 长度 | — | 不统一：标准 K线 6s，SP K线/竞价 22s，资金流 8s，扩展历史成交 43s |

---

## Mandatory Reading

| 优先级 | 文件 | 为何读 |
|---|---|---|
| P0 | `/home/li/peiking88/opentdx/opentdx/parser/ex_quotation/*` | 扩展行情 20 parser 逐字移植（kline/list/quotes/history_transaction/tick_chart/server） |
| P0 | `/home/li/peiking88/opentdx/opentdx/parser/mac_quotation/*` | SP 14 parser（board_list/board_members_quotes/symbol_bar/unusual/symbol_capital_flow/symbol_auction/symbol_quotes） |
| P0 | `/home/li/peiking88/opentdx/opentdx/utils/bitmap.py` | 位图机制核心（FIELD_BITMAP_MAP 全表 + build_bitmap + PresetField） |
| P0 | `/home/li/peiking88/opentdx/opentdx/utils/help.py:233-363` | unpack_futures（扩展报价）+ unpack_by_type（异动类型表）+ exchange_board_code/combine_to_datetime |
| P0 | `/home/li/peiking88/opentdx/opentdx/utils/block_reader.py` | block.dat 本地板块（2800B/块） |
| P1 | `/home/li/peiking88/opentdx/opentdx/const.py:181-544` | mac_hosts/mac_ex_hosts/ex_hosts + EX_MARKET + EX_CATEGORY + BOARD_TYPE |
| P1 | `/home/li/peiking88/opentdx/opentdx/client/exQuotationClient.py` + `macQuotationClient.py` + `commonClientMixin.py` | 客户端 API + sp() 切换 |
| P1 | Phase 1 已落地代码（connection/frame/codec） | 复用范式 |

---

## Patterns to Mirror

**PATTERN 1：扩展行情帧**（head=1）
```cpp
// 扩展行情所有请求 head=1（非 0x0c）。pack_request 第 1 参数传 0x01。
auto req = pack_request(0x01, 0, kMsgExKline, body.data(), body.size());
```

**PATTERN 2：扩展 K 线 float32 定点（不复用标准 get_price）**
```cpp
// ex_kline 响应每根 32B：<IfffffIf>（date + OHLC + amount float + vol uint32 + _ float）
// 与标准 K 线（get_price 变长）完全独立，新写 deserialize_ex_kline。
```

**PATTERN 3：unpack_futures 多段报价**（291+code_len 字节）
```cpp
// 分 4 段：头(B+code_len) / 行情核心(60B) / 盘口(80B→5档bid/ask) / 中段(38B) / 尾段(112B)
// 盘口重组：list[0..4]=bid价 [5..9]=bid量 [10..14]=ask价 [15..19]=ask量
```

**PATTERN 4：SP 位图动态字段**（0x122B/0x122C）
```cpp
// 请求末尾追加 20 字节位图（build_bitmap）；响应前 20 字节回传位图 + 6 字节 <IH>(total,count)
// 行长 = 68 + 4*popcount(bitmap)；按 bit 升序，每激活位按 FIELD_BITMAP_MAP[bit].fmt 读 4 字节
```

**PATTERN 5：SP 0x1218 双业务 dispatcher**（靠内嵌 ASCII 串）
```cpp
// 同一 msg_id 0x1218：请求体含 "Stock_ZJLX"→资金流；含 "Stock_GLHQ"→所属板块
// C++ 按 (msg_id, 内嵌串) 分发，不能只按 msg_id
```

**PATTERN 6：fiber 纪律**（沿用 Phase 1）+ `::util::` 全限定（避免命名空间歧义）

---

## Files to Create / Update

| 文件 | 动作 | 说明 |
|---|---|---|
| `include/tdx/consts.hpp` | UPDATE | EX_MARKET/EX_CATEGORY/BOARD_TYPE 枚举 + mac_hosts 常量 |
| `cfg/servers.json` | UPDATE | 已含 mac_hosts/mac_ex_hosts（Phase 1 提取），无需改 |
| `include/tdx/proto/ex_parsers.hpp` + `src/proto/ex_parsers*.cpp` | CREATE | 扩展行情 parser（kline/list/quotes/category/history_txn/tick/login） |
| `include/tdx/proto/ex_connection.hpp` + `.cpp` | CREATE | ExtConnection（7727，head=1，不发心跳） |
| `include/tdx/proto/sp_parsers.hpp` + `src/proto/sp_parsers*.cpp` | CREATE | SP parser（board/board_members/symbol_bar/unusual/capital_flow/auction） |
| `include/tdx/proto/bitmap.hpp` + `.cpp` | CREATE | 位图机制（FIELD_BITMAP_MAP + build_bitmap + PresetField） |
| `include/tdx/proto/sp_codec.hpp` + `.cpp` | CREATE | SP 工具（exchange_board_code/combine_to_datetime/unpack_by_type） |
| `include/tdx/proto/block_reader.hpp` + `.cpp` | CREATE | block.dat 本地板块（2800B/块）+ CustomerBlock |
| `include/nlohmann/json.hpp` | CREATE | vendored single-header JSON（用于 0x1218 响应） |
| `include/tdx/quotes/ext_quotes.hpp` + `.cpp` | CREATE | ExtQuotes API |
| `include/tdx/quotes/sp_quotes.hpp` + `.cpp` | CREATE | SPQuotes API（含 sp() 切 mac_hosts） |
| `src/cli/main.cpp` | UPDATE | 加子命令 ex-bars/ex-quotes/sp-boards/sp-capital-flow/sp-blocks |
| `tests/test_ex_parsers.cpp` + `test_sp_parsers.cpp` + `test_bitmap.cpp` + `test_block_reader.cpp` | CREATE | 单元测试 |
| `tests/fixtures/golden/ex_*` + `sp_*` | CREATE | 黄金字节流 fixtures |
| `scripts/record_golden.py` | UPDATE | 加 ex + SP 录制（opentdx exQuotationClient/macQuotationClient） |

---

## NOT Building（Phase 2 范围外）

- **4 个未逆向扩展 parser**（ex_quotation/goods.py 的 0x23f6/0x2487/0x2488/0x2562）——上游源码 TODO，响应格式未确认，跳过
- **扩展行情 Table/TableDetail 文本表格**（0x2422/0x2423）——低优先级，Phase 3 再评估
- **InstrumentInfo/ChartSampling/HistoryInstrumentBarsRange**（tdxpy 兼容）——非核心，Phase 3
- **数据管理层**（TdxData/Sync/复权/重采样）→ Phase 3
- **并发批量/落盘存储** → Phase 4 (v2)

---

## Step-by-Step Tasks

### 基础（共享）

#### Task 1: 扩展枚举 + SP 工具函数
- **ACTION**: `consts.hpp` 加 EX_MARKET/EX_CATEGORY/BOARD_TYPE/SORT_TYPE 枚举；新建 `sp_codec.{hpp,cpp}`（exchange_board_code/combine_to_datetime/industry_to_board_symbol）
- **MIRROR**: opentdx const.py:243-278（EX_CATEGORY）、:470-481（BOARD_TYPE）、:491-544（EX_MARKET）；help.py:55-134
- **GOTCHA**: EX_MARKET 值域 1-102，与标准 MARKET(0-2) 独立；exchange_board_code 转换 880xxx→20xxx/HK→20xxx/US→30xxx
- **VALIDATE**: 编译 + 单元测试（枚举值与上游一致、exchange_board_code 转换）

#### Task 2: JSON 库引入
- **ACTION**: vendored `include/nlohmann/json.hpp`（从国内镜像 ghfast.top 下），或手写最小 JSON 数组解析（仅支持 0x1218 的嵌套数组）
- **GOTCHA**: 0x1218 响应是 GBK 编码的 JSON 数组（非对象），需先 GBK→UTF8 再解析；nlohmann 是 single-header（~800KB），全局规范"简单优先"——若仅 0x1218 用，手写更轻
- **VALIDATE**: 编译 + 单元测试（解析 GBK JSON 数组）

### 扩展行情（端口 7727）

#### Task 3: ExtConnection + Login（head=1）
- **ACTION**: `ex_connection.{hpp,cpp}`（端口 7727，head=1，**不发心跳**）+ Login（0x2454，80 字节 hex 常量，从 ex_quotation/server.py:7-19 逐字节复制）
- **MIRROR**: Phase 1 Connection（去掉 Heartbeat）+ opentdx exQuotationClient（不发心跳）
- **GOTCHA**: head=1（非 0x0c）；Login body 是 80B 固定 hex，硬编码常量数组；ServerPool 用 ex_hosts（7727）
- **VALIDATE**: live 测试（连 ex_hosts，登录成功）

#### Task 4: 扩展 K线 Parser（float32 定点）
- **ACTION**: `ex_parsers.hpp/cpp` 加 serialize/deserialize_ex_kline（0x23ff `<B9sHHIH` 请求，响应 32B/根 `<IfffffIf>`）+ deserialize_ex_kline2（0x2489 `<B23sHHII16x` 请求，响应 `<IfffffII>`）
- **MIRROR**: opentdx ex_quotation/kline.py + kline2.py
- **GOTCHA**: **不复用**标准 deserialize_kline（标准用 get_price 变长，扩展用 float32 定点）；code 9s(0x23ff)/23s(0x2489) GBK；无 adjust 参数；date_num 用 to_datetime(minute_category)
- **VALIDATE**: 黄金字节流（录制 ex K线 fixture，C++ 解析对齐）

#### Task 5: 扩展列表/报价 Parser
- **ACTION**: serialize/deserialize_ex_list（0x23f5 `<IH` 请求，64B/条）、ex_category_list（0x23f4 空 body，64B/条）、ex_count（0x23f0）；unpack_futures（291+code_len，4 段）+ Quotes（0x248a）/QuotesSingle（0x23fa）/QuotesList（0x2484）
- **MIRROR**: opentdx ex_quotation/list.py/category_list.py/count.py/quotes.py/quotes_single.py + help.py:233-287（unpack_futures）
- **GOTCHA**: unpack_futures 盘口重组 list[0..4]=bid价 [5..9]=bid量 [10..14]=ask价 [15..19]=ask量；code_len=23（Quotes）/9（QuotesSingle）；Quotes 请求头 `<B7xH`（5,7填充,length）
- **VALIDATE**: 黄金字节流（ex 列表 + 报价 fixture）

#### Task 6: 扩展历史成交 + TickChart
- **ACTION**: serialize/deserialize_ex_history_txn（0x2412 `<IB43sH` 请求，16B/笔 `<HIIIH`，price 是 uint32）+ TickChart（0x248b/0x248c）
- **MIRROR**: opentdx ex_quotation/history_transaction.py + tick_chart.py + history_tick_chart.py
- **GOTCHA**: 历史成交 code 43s GBK + 尾部 0x78；price uint32（非 float）；buy_sell {0:BUY,1:SELL,2:NEUTRAL}
- **VALIDATE**: 黄金字节流（ex 历史成交 fixture）

#### Task 7: ExtQuotes API + CLI
- **ACTION**: `ext_quotes.{hpp,cpp}`（组合 ExtConnection + Retry/CircuitBreaker + ex_parser）：方法 Bars/Quotes/Stocks/CategoryList/HistoryTransactions；CLI 加 `ex-bars <market> <code> <period> <count>` / `ex-quotes`
- **MIRROR**: Phase 1 StdQuotes（结构同构）+ opentdx exQuotationClient
- **GOTCHA**: market 是 EX_MARKET（非标准 MARKET）；不发心跳
- **VALIDATE**: live 测试（拉期货/港美股 K线，如 HK_MAIN_BOARD 港股、CFFEX_FUTURES 期货）

### SP/MAC 高级行情（mac_hosts）

#### Task 8: SP 模式 + mac_hosts 切换
- **ACTION**: `sp_quotes.{hpp,cpp}`（SPQuotes 类：组合标准 Connection + mac_hosts 切换 + Retry/CircuitBreaker）；sp() 方法切 mac_hosts；登录用标准 0x0d
- **MIRROR**: opentdx macQuotationClient + commonClientMixin.sp()；mootdx _get_sp_client
- **GOTCHA**: SP 帧头与标准一致（head=0x0c）；mac_hosts 端口 7709（IP 不同）；不同 IP 板块总数不同（559 vs 488）
- **VALIDATE**: live 测试（连 mac_hosts，登录成功）

#### Task 9: 位图机制
- **ACTION**: `bitmap.{hpp,cpp}`：FieldBit 枚举（160 位）+ FIELD_BITMAP_MAP（field_name/fmt/描述，从 bitmap.py:5-105 全表）+ build_bitmap(fields)→20 字节 + PresetField（BASIC/QUOTE/COMMON/ALL 等预设集）
- **MIRROR**: opentdx utils/bitmap.py:5-333
- **GOTCHA**: 20 字节小端；解析按 bit 升序；行长=68+4*popcount；未知位生成 unknown_field_{bit}（<f，接近 0 回退 <i）
- **VALIDATE**: 单元测试（build_bitmap 编码 + 各 PresetField 位组合 + 响应解析动态字段）

#### Task 10: SP 板块 Parser（位图驱动）
- **ACTION**: `sp_parsers.hpp/cpp` 加 BoardList（0x1231 `<HHBBHH8x` 请求，160B/块含 board+symbol 双结构）、BoardMembersQuotes（0x122C 继承 SymbolQuotes 位图）、SymbolQuotes（0x122B 位图响应）
- **MIRROR**: opentdx mac_quotation/board_list.py + board_members_quotes.py + symbol_quotes.py
- **GOTCHA**: BoardMembersQuotes 请求末尾追加 20B 位图；board_code 经 exchange_board_code 转换；响应前 20B 回传位图
- **VALIDATE**: 黄金字节流（SP 板块列表 + 成员报价 fixture）

#### Task 11: SP K线 + 异动
- **ACTION**: SymbolBar（0x122E `<H22sHHIHHbbbH4s` 请求，22s code，响应 36B/根 `<II7f`）+ Unusual（0x1237 `<HH2xH2xH5H` 请求，32B/条 + unpack_by_type 异动类型表 + 尾部 GBK 文本段）
- **MIRROR**: opentdx mac_quotation/symbol_bar.py + unusual.py + help.py:289-363（unpack_by_type 20+ 异动类型表）
- **GOTCHA**: SP K线 code 22s（非 6s）；日期 combine_to_datetime（ymd + 当日秒数）；异动不需 Login；unpack_by_type 逐类型 switch 移植
- **VALIDATE**: 黄金字节流（SP K线 + 异动 fixture）

#### Task 12: SP 资金流/所属板块/竞价（JSON）
- **ACTION**: SymbolCapitalFlow（0x1218 + "Stock_ZJLX"，响应 27B 头 + GBK JSON 数组，10 列 + 3 派生净额）、SymbolBelongBoard（0x1218 + "Stock_GLHQ"，9 或 13 列变体）、Auction（0x123D `<H22sII10x` 请求，16B/条 `<IfIi`）
- **MIRROR**: opentdx mac_quotation/symbol_capital_flow.py + symbol_belong_board.py + symbol_auction.py
- **GOTCHA**: 0x1218 双业务靠内嵌 ASCII 串分发；JSON 响应先 GBK→UTF8 再 nlohmann 解析；today 4 元素 + five_days 6 元素合并 10 列
- **VALIDATE**: 黄金字节流（资金流 + 竞价 fixture）

#### Task 13: BlockReader（本地板块）
- **ACTION**: `block_reader.{hpp,cpp}`：读 block.dat/block_zs.dat/block_fg.dat/block_gn.dat（384B 头 + `<H`num + 每块 9B 名 + `<HH` + N×7B code UTF8，固定 2800B/块）+ CustomerBlock（blocknew.cfg 120B/条 + .blk 文本）
- **MIRROR**: opentdx utils/block_reader.py:23-135
- **GOTCHA**: 股票代码是 UTF-8（非 GBK）；每块固定 2800B（跳到块边界）；BLOCK_FILE_TYPE 枚举
- **VALIDATE**: local 测试（读真实 block.dat，板块成员与通达信一致）

### 集成 / 验证

#### Task 14: 黄金录制扩展 + fixtures + 测试 + live
- **ACTION**: record_golden.py 加 ex（连 ex_hosts）+ SP（连 mac_hosts）录制；生成 ex_kline/ex_quotes/sp_boards/sp_capital_flow/sp_unusal fixtures；test_ex_parsers/test_sp_parsers/test_bitmap/test_block_reader + golden 测试；CLI live（ex-bars 期货 + sp-boards 板块 + sp-capital-flow 资金流）
- **VALIDATE**: 全套测试通过 + live（期货/港美股 K线 + 板块列表 + 资金流数据合理）

---

## Testing Strategy

### 单元测试
| 测试 | 覆盖 |
|---|---|
| test_ex_parsers | ex K线 float32、列表、unpack_futures 报价、历史成交 |
| test_sp_parsers | SP K线 22s code、异动 unpack_by_type、资金流 JSON、竞价 |
| test_bitmap | build_bitmap 编码、FIELD_BITMAP_MAP 全位、PresetField、响应动态解析 |
| test_block_reader | block.dat 2800B/块解析、CustomerBlock |
| test_sp_codec | exchange_board_code/combine_to_datetime 转换 |

### 黄金字节流（沿用 Phase 1 record_golden.py 模式）
- ex_kline_HK（港股）/ ex_kline_CFFEX（期货）/ ex_quotes / ex_history_txn
- sp_boards（板块列表）/ sp_board_members（成员报价位图）/ sp_capital_flow（资金流 JSON）/ sp_unusal（异动）

### live 测试（间加延迟 + CircuitBreaker 保护）
- 扩展行情：拉 HK_MAIN_BOARD 港股 / CFFEX_FUTURES 期货 K线
- SP：板块列表（559）、资金流（600000）、异动监控

### Edge Cases
- [ ] 位图未知位（unknown_field_{bit}）容错
- [ ] 0x1218 双业务分发（Stock_ZJLX vs Stock_GLHQ）
- [ ] JSON 响应 GBK 解码失败容错
- [ ] ex 登录 80B hex 完整性
- [ ] block.dat 块边界对齐

---

## Validation Commands

```bash
# 编译（指定 target 避免 build all）
cmake --build build --target tdx tdx_smoke test_ex_parsers test_sp_parsers test_bitmap test_block_reader test_sp_codec -j$(nproc)
# 全量测试
ctest --test-dir build -R "^test_" -j$(nproc) --output-on-failure
# live（间加延迟）
./build/bin/tdx ex-bars 31 00700 4 10      # 港股腾讯日K（HK_MAIN_BOARD=31）
./build/bin/tdx ex-bars 47 IF2406 4 10     # 期货日K（CFFEX=47，示例）
./build/bin/tdx sp-boards                   # 板块列表
./build/bin/tdx sp-capital-flow 600000      # 资金流
```

---

## Acceptance Criteria（对齐 PRD Phase 2 Success Signal）

- [ ] ① 扩展行情 live 测试拉到期货/港美股 K线，数据合理
- [ ] ② SP 板块列表（~559）/资金流可获取
- [ ] ③ 与上游等价（黄金字节流 C++ 解析对齐 opentdx）
- [ ] ④ 位图机制全表测试通过（160 位）
- [ ] ⑤ block.dat 本地解析与通达信一致
- [ ] ⑥ 全部单元 + golden + live 测试通过
- [ ] ⑦ fiber 纪律无违规（沿用 Phase 1）

---

## Risks and Mitigations

| 风险 | 可能性 | 影响 | 缓解 |
|---|---|---|---|
| 扩展行情登录 80B hex 抄错 | 中 | 登录失败 | 逐字节复制 server.py:7-19，hex 比对 |
| 位图 FIELD_BITMAP_MAP 漏位 | 中 | 字段缺失 | 全表测试（160 位遍历） |
| 0x1218 JSON 解析（GBK + 嵌套数组） | 高 | 资金流失效 | nlohmann + GBK→UTF8；专项测试 |
| 异动 unpack_by_type 类型表（20+） | 中 | 异动描述错 | 逐类型 switch，黄金字节流覆盖 |
| 扩展行情 head=1 易漏（误用 0x0c） | 中 | 服务器无响应 | pack_request 强制 head 参数 |
| 4 个未逆向 parser（0x23f6 等） | — | — | NOT Building，跳过 |
| mac_hosts 板块总数 IP 差异（559/488） | 低 | 选服选到少数据 IP | ServerPool 测速 + 文档标注 |
| nlohmann 体积（~800KB） | 低 | 编译变慢 | 备选手写最小 JSON（仅 0x1218） |

---

## Notes

- **依赖关键路径**：Task 1-2（基础）→ [Task 3-7 扩展 ‖ Task 8-13 SP] → Task 14 集成。扩展与 SP 两条链可并行（不同 Connection/hosts）。
- **不复用**：扩展 K 线不复用标准 deserialize_kline（编码不同）；SP Connection 复用标准（同帧，切 hosts）。
- **CLAUDE.md 需更新**：本计划「关键修正」表的 head/customize/登录描述应回写 CLAUDE.md（Phase 2 完成时）。
- **版本号**：Phase 2 完成 → 0.2.0（增加功能升次版本号）。

---

*下一步：评审本计划 → 运行 `/prp-implement .claude/PRPs/plans/phase2-extended-sp-quotes.plan.md` 执行 Phase 2。*
