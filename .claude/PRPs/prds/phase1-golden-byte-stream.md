# Phase 1 黄金字节流测试细化方案

> 评审对象：Phase 1 协议层二进制帧编解码的正确性验收方法
> 父文档：`/home/li/peiking88/tdx-cpp/.claude/PRPs/prds/tdx-cpp.prd.md`（Open Question #10 → 本方案单独评审）
*Generated: 2026-06-22 | Status: DRAFT — 待评审*

---

## 一、目的与定义

### 什么是「黄金字节流测试」
给定一段**真实的通达信协议字节流**（从上游 Python 实现或真服录制），C++ 解析器的输出必须与**上游 Python 解析结果逐字段一致**。这是移植类项目最硬的正确性兜底——不通过则协议层不可信。

### 为什么需要它
通达信响应体大量使用**变长编码（get_price）+ 增量编码 + 智能字段检测 + fallback 解码**，C++ 移植极易在以下点出错，单元测试的「构造-断言」不足以覆盖：
- 增量累加顺序错（逐笔 `last_price += price`、分时 `start_price` 累加）
- OHLC 字段顺序错（K线是 `open/close/high/low`，非 `open/high/low/close`）
- 五档/报价的「相对 price 增量」漏加基准
- 变长价格的符号位/继续位/多字节移位
- to_datetime 的 YYYYMMDD ↔ 紧凑编码 fallback
- K线 upCount/downCount 的「试探下一字节是日期还是计数」智能检测

### 关键约束（已确认）
**opentdx 的 `tests/` 目录无 golden fixtures**（`.dat/.bin/.bytes` 均不存在）——C++ **不能直接复用上游字节流文件，必须自行录制**。这是本方案方法论的基础前提。

---

## 二、核心难点登记（测试必须命中）

| # | 难点 | 上游位置 | 测试要求 |
|---|---|---|---|
| D1 | **增量编码**：逐笔 price 累加、分时 price/avg 累加 | transaction.py:32, tick_chart.py:32-35 | 多条记录链路验证，不能只测单条 |
| D2 | **相对基准增量**：报价 open/high/low/pre_close 与五档 bid/ask 都 `+= price` | quotes_detail.py:57-58,76-79 | 基准 price 必须先解出再加 |
| D3 | **OHLC 顺序**：K线 `open→close→high→low`（反直觉） | kline.py:38-47 | 字段映射断言 |
| D4 | **变长价格 get_price**：bit0x40 符号、bit0x80 继续、每后续 7bit | help.py:137-169 | 正/负/单字节/多字节边界全覆盖 |
| D5 | **to_datetime fallback**：YYYYMMDD ↔ 紧凑编码互转 + 越界回退 | help.py:171-207 | 两种格式 + 越界用例 |
| D6 | **K线 upCount/downCount 智能检测**：试探后 4 字节是日期还是计数 | kline.py:56-75 | 有/无 upCount 两种响应各测 |
| D7 | **zlib 解压触发**：zipsize≠unzip_size | baseStockClient.py:310-319 | 压缩/未压缩响应各测 |
| D8 | **prefix 校验**：响应头 `b1 cb 74 00` | baseStockClient.py:299-308 | 错误 prefix 抛异常 |

---

## 三、方法论

### 字节流来源（三档，优先级递减）
1. **真服录制（首选）**：Phase 1 live 测试连真服时，用 helio 的 socket 抓取**原始响应字节流**（解压前 + 解压后各存一份），落盘为 golden 文件。同时用上游 Python（opentdx）对同一请求解析，结果作为 expected 输出。
2. **上游 Python 内联生成**：对已知协议格式，用 Python `struct.pack` 构造响应字节 + 期望输出（适合边界值用例，如 get_price 多字节、to_datetime 越界）。
3. **手工构造异常流**：prefix 错误、zlib 损坏、截断数据，验证 C++ 抛正确异常。

### 存档格式
```
tests/fixtures/golden/
├── kline_600000_day.bin          # 原始响应体（解压后）
├── kline_600000_day.expected.json # 上游 Python 解析结果（期望输出）
├── kline_600000_5min.bin
├── transaction_600000.bin
├── quotes_detail_multi.bin
├── list_sh.bin
└── ...（每个 msg_id × 几个代表性股票/场景）
```
- `.bin`：原始字节（hex 可读）
- `.expected.json`：上游解析的结构化结果（字段名、值、类型），作为 C++ 断言的 expected

### 对比方式
- C++ 解析 `.bin` → 序列化为相同 JSON schema → 与 `.expected.json` **逐字段 deep equal**。
- 浮点用「相对误差 < 1e-6」比较（避免 float 精度差异），不直接 `==`。
- 增量字段（D1/D2）必须验证完整链路，不只末值。

### 录制工具
提供一个 `scripts/record_golden.py`（或 C++ 工具）：对指定 (msg_id, market, code, params) 连真服，抓响应字节 + 调上游解析，自动生成 `.bin` + `.expected.json`。一次录制，长期复用。

---

## 四、msg_id 覆盖清单（Phase 1）

| 优先级 | msg_id | 用途 | 请求体格式 | 响应特征 | 关键难点 |
|---|---|---|---|---|---|
| **P0** | — | 帧层（请求封包/响应解包） | `<BIBHH` 请求头 | `<IBIBHHH` 响应头 | D7, D8 |
| **P0** | 0x523 | K线（核心） | `<H6sHHHHH8s` | count + 变长 OHLC + vol/amount | D3, D4, D6 |
| **P0** | 0xfc5 | 逐笔成交 | `<H6sHH` | count + 变长(5字段) | D1, D4 |
| **P0** | 0x537 | 分时图 | `<H6sHH` | num + 变长(3字段) | D1, D4 |
| **P0** | 0x53e | 报价详情（五档） | `<H6sH` + N×`<B6s` | count + 复杂结构 | D2, D4 |
| **P0** | 0x44d | 股票列表 | `<H3I` | count + 37B/条固定 | 固定格式 |
| **P1** | 0x44e | 股票数量 | （见 count.py） | 数量 | 简单 |
| **P1** | 0x0d | 登录 | `<B`(=1) | 登录结果 | 状态判断 |
| **P1** | 0x04 | 心跳 | 空/简单 | 空/确认 | 时序（另见 fiber 时序测试） |
| **P1** | 0x0f | 除权除息 | （实现时确认） | xdxr 事件流 | 字段映射（C16 复权依赖） |
| **P2** | 0x10 | 财务 | （见 finance.py） | 财务字段 | 字段多 |
| **P2** | 0x2cf/0x2d0 | F10 目录/内容 | （见 company_info.py） | 文本 | GBK 解码 |
| **专项** | — | get_price 变长解码 | 构造字节序列 | 整数值 | D4 全边界 |
| **专项** | — | to_datetime 日期解码 | 构造整数 | datetime | D5 全分支 |

**P0 必须在 Phase 1 验收前全过**；P1 应过；P2 可延后至用到时补。

---

## 五、核心 msg_id 详细测试设计

### 5.1 帧层（P0，D7/D8）

**请求封包**：
- 用例：给定 (head=0x0c, customize=0, msg_id=0x523, payload=...) → 断言封包字节 = `<BIBHH`(head,customize,1,len,len) + `<H`(msg_id) + payload，与上游 `baseParser.py:9-25` 逐字节一致。
- 用例：head=0x1c（压缩请求）。

**响应解包**：
- 用例：响应头 16B `<IBIBHHH` → prefix=`b1 cb 74 00`（否则抛 `TdxProtocolError`）、zipped/customize/msg_id/zipsize/unzip_size 正确解析。
- 用例 D7：zipsize≠unzip_size → zlib 解压 body；zipsize==unzip_size → 不解压。
- 用例 D8：prefix 错误（如 `b1 cb 74 01`）→ 抛异常，不静默。
- 用例：zlib 损坏 body → 抛异常。

### 5.2 0x523 K线（P0，D3/D4/D6）

**请求构造**：`struct.pack('<H6sHHHHH8s', market, code_gbk, period, times, start, count, adjust, b'')`。
- 用例：market=SH(1), code='600000', period=日(9), count=800, adjust=NONE。断言请求字节与上游一致。

**响应解析**：
- 字段顺序：date_num(I) → open(get_price) → **close**(get_price) → **high**(get_price) → **low**(get_price) → vol/amount(`<ff`)。
- 用例 D3：构造一条 K线响应，断言 open/close/high/low 映射正确（**重点验证 close 在 high 前**）。
- 用例 D4：OHLC 用变长编码，覆盖正价、含负值的 high/low（如 low 相对 open 为负）。
- 用例 D6-upCount：响应含 upCount/downCount（后 4B 是计数而非日期）→ 解析出 up_count/down_count。
- 用例 D6-noUpCount：响应不含 upCount（后 4B 是下一根 bar 的日期）→ 不误读，bars 数量正确。
- 用例：日K date_num=YYYYMMDD（如 20240101）→ to_datetime 正确。
- 用例：分钟线 date_num=紧凑编码 → to_datetime(with_time=True) 正确。
- 用例：count=0 → 返回空 vector 不崩溃。
- 用例：响应截断（pos+len>data_len）→ 安全停止，返回已解析的 bars（对齐 kline.py:29 的 break 行为）。

### 5.3 0xfc5 逐笔（P0，D1/D4）

**请求**：`<H6sHH`(market, code_gbk, start, count)。

**响应**（D1 增量是核心）：
- 每条：minutes(H) → price/vol/trans/buy_sell/unknown（各 get_price）。
- **price 是增量**：`last_price += price`（transaction.py:32）。
- 用例 D1：构造 3 条逐笔，price 增量分别为 +100/-50+200（注意符号），断言每条的累计 last_price 与上游一致。**不能只验末值**。
- 用例 D4：buy_sell 映射 {0:BUY,1:SELL,2:NEUTRAL}。
- 用例：minutes 跨小时（如 13:01 = 781 分钟）→ time(13,1) 正确。

### 5.4 0x537 分时（P0，D1/D4）

**请求**：`<H6sHH`(market, code_gbk, start, count=0xba00)。

**响应**（D1 双增量）：
- 每条：price/avg/vol（各 get_price）。
- **price 和 avg 都是增量**：首条后 start_price/avg 锁定，后续相对累加（tick_chart.py:28-35）。
- 用例 D1：构造 3 条分时，验证 price/avg 的增量累加与上游逐条一致。
- 用例：num=0 → 空结果。

### 5.5 0x53e 报价详情/五档（P0，D2/D4）

**请求**：`<H6sH`(5,'',count) + 每股票 `<B6s`(market, code_gbk)。

**响应**（D2 相对基准增量是核心）：
- 头部：market/code/active1(`<B6sH`, 9B)。
- price(get_price) ← **基准**，后续多个字段相对它增量。
- pre_close/open/high/low/server_time/neg_price/vol/cur_vol（各 get_price）。
- amount(`<f`, 4B)。
- s_vol/b_vol/s_amount/open_amount（各 get_price）。
- 五档 ×5：每档 bid/ask/bid_vol/ask_vol（各 get_price），**bid/ask 都 `+= price`**（quotes_detail.py:57-58）。
- 尾部：unknown/rise_speed/active2(`<h4shH`, 10B)。
- **open/high/low/pre_close 都 `+= price`**（quotes_detail.py:76-79）。
- 用例 D2：构造 1 只股票完整报价，断言 open=price+open_raw、high=price+high_raw、五档 bid=price+bid_raw 全部正确。
- 用例：5 档中部分档位缺失（盘前）→ sentinel 处理（关联 PRD P2-3 null 语义）。
- 用例：多股票（count=3）→ 每只独立解析，pos 正确推进。

### 5.6 0x44d 列表（P0）

**请求**：`<H3I`(market, start, count, 0)。

**响应**（固定 37B/条）：
- `<6sH16sfBfHH`：code(6s gbk) + vol(H) + name(16s gbk) + unknown1(f) + decimal_point(B) + pre_close(f) + unknown2(H) + unknown3(H)。
- 用例：code/name 用 GBK 解码 + rstrip `\x00`。
- 用例：count=1600（满页）→ 全部解析，pos 推进正确。
- 用例：中文股票名（如「浦发银行」）→ iconv GBK 正确。

### 5.7 0x0f 除权除息（P1，实现时确认字段）

- **待确认**：上游 0x0f 的 parser 文件位置（grep 显示 xdxr 出现在 company_info.py 与 __init__.py，可能内联于 client 而非独立 parser）。
- 实现时定位精确请求/响应格式后补本节。**关键**：xdxr 事件流是 Phase 3 复权计算（C16）的输入，字段映射必须准确。
- 用例：含送股/分红/配股的 xdxr 事件 → 各字段（send/dividend/rationed/rationed_price/date）正确。

### 5.8 0x0d 登录 / 0x04 心跳（P1）

- **0x0d 登录**（server.py:91-92）：请求体 `<B`(=1)。响应 → 判断登录成功标志。
- **0x04 心跳**（server.py:19）：请求/响应简单。心跳的**时序**（15s 触发、20 次无业务断开）归 fiber 异步时序测试（见 PRD 测试策略），本节只验帧编解码。

---

## 六、变长解码器专项测试（D4/D5）

### 6.1 get_price（help.py:137-169）全边界

| 输入字节 | 期望 int_data | 覆盖 |
|---|---|---|
| `0x05` | 5 | 单字节正数 |
| `0x45` | -5 | 单字节负数（bit0x40） |
| `0x80 0x01` | 64 | 两字节（bit0x80 继续，第二字节 <<6） |
| `0xC0 0x80 0x01` | 多字节正 | 三字节链 |
| `0xFF 0x7F` | 多字节边界 | 最大 7bit 填充 |
| pos>=data_len | 0, pos+1 | 越界保护 |

**断言**：C++ get_price 与上游对同一字节序列返回相同 (int_data, pos)。

### 6.2 to_datetime（help.py:171-207）全分支

| 输入 num | with_time | 期望 | 覆盖 |
|---|---|---|---|
| 20240101 | False | 2024-01-01 15:00 | 日K YYYYMMDD |
| 紧凑编码 | True | 正确年月日+时分 | 分钟线低16位+高16位 |
| 越界月日(YYYYMMDD 形式但 month=13) | False | fallback 到紧凑解码 | D5 fallback |
| 紧凑解码越界 | True | fallback 到 YYYYMMDD | D5 反向 fallback |

**断言**：C++ to_datetime 与上游返回相同 datetime（含 fallback 分支选择一致）。

---

## 七、字节流录制与维护流程

1. **首次录制**（Phase 1 实现中期）：用 `scripts/record_golden.py` 对 P0 msg_id × 代表性股票（600000 沪/000001 深）连真服录制，生成 `.bin` + `.expected.json`。
2. **入库**：golden 文件提交 git（不大，每个几 KB），作为测试基线。
3. **回归**：每次协议层改动，CI 跑全量 golden 测试，任一不一致即失败。
4. **更新**：协议变更（如通达信改协议）时，重新录制并更新 expected，提交时注明原因。
5. **隐私**：录制的是行情数据（非账户信息），可入库；但录制脚本不硬编码任何敏感配置。

---

## 八、验收标准

- [ ] P0 msg_id（帧层/0x523/0xfc5/0x537/0x53e/0x44d）golden 测试**全部通过**，C++ 输出与上游 expected 逐字段一致（浮点相对误差<1e-6）。
- [ ] get_price / to_datetime 专项测试全边界通过。
- [ ] 增量字段（D1/D2）验证完整链路，非仅末值。
- [ ] 异常流（prefix 错/zlib 损坏/截断）抛正确异常，不静默。
- [ ] golden `.bin`/`.expected.json` 入库，CI 回归纳入。
- [ ] 录制工具 `scripts/record_golden` 可用，文档化。

---

## 九、风险与待确认（评审重点）

| # | 项 | 说明 | 建议 |
|---|---|---|---|
| R1 | **无上游 fixtures** | opentdx tests/ 无 golden 文件，全部需自录 | 接受，录制工具一次性投入 |
| R2 | **真服录制依赖 live** | 录制需连真服，可能被限流 | 录制时加延迟；或用 Python 内联构造覆盖边界值 |
| R3 | **0x0f xdxr 格式待定位** | 上游可能内联于 client 而非独立 parser | 实现时定位 server.py/相关 client 确认 |
| R4 | **0x547 vs 0x53e** | 五档报价有多个 msg_id（0x53e 详情/0x547/0x54b/0x54c） | golden 优先覆盖 0x53e（字段最全），其余实现时对照 |
| R5 | **浮点精度** | 上游 Python float vs C++ double | 用相对误差<1e-6 比较，不直接 == |
| R6 | **响应字段顺序脆弱** | 上游用 get_price 顺序读取，C++ 必须严格复刻顺序 | 黄金测试天然覆盖（顺序错则全错） |

---

*评审通过后，本方案作为 Phase 1 实施计划（`/prp-plan`）的输入之一，golden 测试用例纳入 Phase 1 Success Signal ③。*
