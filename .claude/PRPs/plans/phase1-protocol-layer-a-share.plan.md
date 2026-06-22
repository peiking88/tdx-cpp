# Feature: Phase 1 — 协议层 + A股标准行情核心

## Summary

基于 helio（io_uring+fiber）搭建 tdx-cpp 协议层骨架：实现通达信 TCP 二进制帧编解码、连接管理（心跳/自动选服/重连）、RetryPolicy+CircuitBreaker 弹性层、命令号→解析器注册表，以及 A 股标准行情的全部核心 Parser（K线/分时/逐笔/五档/列表/除权/财务）与本地 vipdoc 文件读取。完成后能用 helio 连接真服、在熔断保护下拉取 600000 日K并与上游字节级一致，能读取本地 `.day` 文件，全部黄金字节流测试通过。

## User Story

**作为** A 股量化工程师（核心代码为 C++），
**我想要** 一个能链接进 C++ 进程的通达信行情库，用原生 helio 异步 IO 连服务器、在弹性保护下获取 A 股 K线/五档/逐笔/除权数据，
**以便** 消除 Python 运行时依赖、获得确定性性能与协议正确性。

## Problem Statement

通达信 TCP 二进制协议的 C++ 实现散落且不完整；移植上游 opentdx 的 Python 实现到 C++ 时，变长编码、增量编码、智能字段检测极易出错。Phase 1 必须用黄金字节流测试逐字段对齐上游，证明协议移植正确——这是整个库可信的地基。

## Solution Statement

三层分层（协议层 `tdx::proto` / 行情接口层 `tdx::quotes::StdQuotes` / 基础设施 `tdx::util`），协议层用 helio `FiberSocketBase` 做 TCP、`AddPeriodic` 做心跳、`MakeFiber` 做并发测速；帧编解码逐字移植 opentdx `baseParser.py`/`baseStockClient.py`；Parser 注册表 + 黄金字节流测试保证字节级正确。

## Metadata

| Field | Value |
|---|---|
| Type | NEW_CAPABILITY（协议层从零搭建） |
| Complexity | HIGH（二进制协议移植 + helio fiber + 弹性层 + 多 Parser） |
| Systems Affected | tdx::util, tdx::proto, tdx::quotes::StdQuotes, CLI 骨架, CMake, tests |
| Dependencies | helio（add_subdirectory 内嵌）、系统 Boost(context+system)、zlib、iconv、GoogleTest |
| Source PRD | `.claude/PRPs/prds/tdx-cpp.prd.md`（Phase 1） |
| Golden Byte Stream | `.claude/PRPs/prds/phase1-golden-byte-stream.md`（Success Signal ③ 输入） |
| Estimated Tasks | 13 |

---

## UX Design（库 API 使用体验，非 UI）

### Before State
```
┌─────────────────────────────────────────────────────────────┐
│  C++ 量化系统需要通达信数据                                    │
│                                                             │
│  方案A: 自研逆向 TDX 协议 ──► 重复造轮子、易错、无测试基线    │
│  方案B: 调 mootdx/tdxdata(Python) ──► IPC/文件喂给 C++        │
│         └─ 需要 Python 运行时 + pandas + 跨进程开销           │
│                                                             │
│  PAIN: 协议知识碎片化、部署重、性能不确定、无权威 C++ 参考     │
└─────────────────────────────────────────────────────────────┘
```

### After State
```
┌─────────────────────────────────────────────────────────────┐
│  C++ 量化系统                                                 │
│      │ #include <tdx/tdx.hpp>                                │
│      │ link libtdx                                           │
│      ▼                                                       │
│  TdxData tdx;  ──► helio ProactorPool 自动启动               │
│      │              └─ ServerPool 并发测速 + config.json 缓存 │
│      │              └─ CircuitBreaker 弹性保护                │
│      ▼                                                       │
│  auto bars = tdx.fetch_history(                              │
│      {"600000"}, "2024-01-01", "2024-06-01", "1d", "qfq");  │
│      │                                                       │
│      ▼                                                       │
│  std::vector<KLine>  ──► 强类型结构体，直接用于回测           │
│                                                             │
│  VALUE: 零 Python 依赖、原生性能、协议字节级正确、弹性自愈    │
└─────────────────────────────────────────────────────────────┘
```

### Interaction Changes
| 位置 | Before | After | 影响 |
|---|---|---|---|
| 数据获取 | Python IPC/自研逆向 | C++ 库直接调用 | 消除运行时依赖、跨进程开销 |
| 协议正确性 | 各自实现、无基线 | 黄金字节流对齐上游 | 字节级可信 |
| 连接稳定性 | 无统一弹性 | CircuitBreaker+Retry+心跳 | 限流自愈、不无限重连打服务器 |

---

## Mandatory Reading

**实现前必须读**（实现 agent 必读）：

| 优先级 | 文件 | 行 | 为何读 |
|---|---|---|---|
| P0 | `/home/li/framework/dragonfly/helio/examples/echo_server.cc` | 385-619 | TCP 客户端 fiber 模式（Driver/TLocalClient），**镜像** |
| P0 | `/home/li/peiking88/opentdx/opentdx/client/baseStockClient.py` | 124-329 | 收发骨架（call/send/_send），**逐字移植** |
| P0 | `/home/li/peiking88/opentdx/opentdx/parser/baseParser.py` | 全 | 请求封包 `<BIBHH`，**逐字移植** |
| P0 | `/home/li/peiking88/tdx-cpp/.claude/PRPs/prds/phase1-golden-byte-stream.md` | 全 | 黄金字节流测试设计 + 6 大难点 + msg_id 字段 |
| P0 | `/home/li/peiking88/tdx-cpp/CLAUDE.md`「helio fiber 编码纪律」节 | 92-110 | fiber 纪律（禁用 std::mutex 等），**强制遵守** |
| P0 | `/home/li/peiking88/opentdx/opentdx/utils/help.py` | 137-207 | get_price/to_datetime 算法，**逐字移植** |
| P1 | `/home/li/framework/dragonfly/helio/CLAUDE.md` | 全 | helio 架构、构建、纪律 |
| P1 | `/home/li/peiking88/opentdx/opentdx/const.py` | 4-193 | 服务器列表、MARKET/PERIOD/ADJUST 枚举 |
| P1 | opentdx 各 Parser | 见黄金字节流方案 §4 | 各 msg_id 字段格式 |

**外部文档**：
| 来源 | 为何需要 |
|---|---|
| `helio/CLAUDE.md` + `helio/examples/` | helio 是 C++ 模式的唯一权威（无 web 文档） |
| opentdx 源码 | 协议层逻辑的移植源（私有协议，无 web 文档） |

---

## Patterns to Mirror

### PATTERN 1: helio TCP 客户端 fiber（**镜像**）
```cpp
// SOURCE: helio/examples/echo_server.cc:404-468 (Driver)
// C++ 协议层 Connection 镜像此模式：
class Connection {
  std::unique_ptr<FiberSocketBase> socket_;
 public:
  Connection(ProactorBase* p) { socket_.reset(p->CreateSocket()); }  // :404-406
  error_code Connect(const tcp::endpoint& ep) {
    return socket_->Connect(ep);  // :417，返回 error_code
  }
  // 发送：socket_->WriteSome(buf) 或 socket_->Write(io::Bytes{...})  :432,477
  // 接收：socket_->Recv(io::MutableBytes(buf,n), 0) 或 Read(...)    :441,479
  // 断连判断：FiberSocketBase::IsConnClosed(ec)                     :514,528
  // 关闭：socket_->Shutdown(SHUT_RDWR); socket_->Close()            :551,553
};
```

### PATTERN 2: helio 并发 fiber（并发测速镜像）
```cpp
// SOURCE: helio/examples/echo_server.cc:580-596 (TLocalClient::Connect)
// ServerPool 并发测速镜像：
vector<Fiber> fbs(hosts.size());
for (size_t i = 0; i < fbs.size(); ++i) {
  fbs[i] = MakeFiber([&, i] {
    ThisFiber::SetName(absl::StrCat("probe/", i));
    // 测 hosts[i] 的 TCP 连接延迟
  });
}
for (auto& fb : fbs) fb.Join();  // 收集结果，选最快
```

### PATTERN 3: opentdx 收发骨架（**逐字移植**）
```python
# SOURCE: opentdx/client/baseStockClient.py:124-136, 271-321
# C++ call() 流程镜像：
#   resp = send(parser.serialize())      # :125 封包+发送+接收
#   return parser.deserialize(resp)      # :129 解析
# _send(data) 镜像：                       # :271-321
#   client.send(data)                     # 发整个封包
#   head = recv(16)                       # 收响应头 RSP_HEADER_LEN=16
#   prefix,zipped,...,zipsize,unzip = unpack('<IBIBHHH', head)  # :307
#   if zipsize != unzip_size: body = zlib.decompress(recv(zipsize))  # :310-319
```

### PATTERN 4: fiber 纪律（**强制**）
```cpp
// SOURCE: CLAUDE.md「helio fiber 编码纪律」+ helio/CLAUDE.md:67-77
// fiber/Proactor 线程内代码（tdx::proto 全部）禁用：
//   std::mutex → util::fb2::Mutex
//   std::condition_variable → util::fb2::CondVar
//   std::this_thread::sleep_for → ThisFiber::SleepFor
// 心跳用：ProactorBase::AddPeriodic(15000, [...])
// 测试代码豁免（gtest 在 Proactor 线程外）
```

---

## Files to Create / Update

| 文件 | 动作 | 说明 |
|---|---|---|
| `CMakeLists.txt` | CREATE | project(C++17) + Boost + add_subdirectory(helio) + 各 target |
| `third_party/helio` | CREATE | git submodule（或 vendored） |
| `include/tdx/types.hpp` | CREATE | KLine/Quote/Tick/Xdxr 结构体（epoch + sentinel） |
| `include/tdx/consts.hpp` | CREATE | MARKET/PERIOD/ADJUST 枚举、服务器列表 |
| `src/util/{gbk,zlib,byte_order,time}.cpp` | CREATE | iconv/zlib/字节序/时间工具 |
| `src/proto/frame.{hpp,cpp}` | CREATE | 请求封包 + 响应解包 |
| `src/proto/codec.{hpp,cpp}` | CREATE | get_price/to_datetime 变长解码 |
| `src/proto/connection.{hpp,cpp}` | CREATE | helio FiberSocket 封装 + 收发 |
| `src/proto/retry.{hpp,cpp}` | CREATE | RetryPolicy + CircuitBreaker |
| `src/proto/heartbeat.{hpp,cpp}` | CREATE | helio AddPeriodic 心跳 |
| `src/proto/server_pool.{hpp,cpp}` | CREATE | MakeFiber 并发测速 + config.json 缓存 |
| `src/proto/parser_registry.{hpp,cpp}` | CREATE | msg_id→Parser 注册表 |
| `src/proto/parsers/*.{hpp,cpp}` | CREATE | 各 msg_id Parser（0x523/0x537/0xfc5/0x53e/0x44d/0x44e/0x0f/0x10/0x0d/0x04/F10） |
| `src/proto/vipdoc_reader.{hpp,cpp}` | CREATE | .day/.lc1/.lc5 读取 |
| `src/quotes/std_quotes.{hpp,cpp}` | CREATE | A股行情 API |
| `src/cli/main.cpp` | CREATE | CLI 骨架（server-test/bars） |
| `tests/*` | CREATE | 单元 + local + live + fiber 时序 + 黄金字节流 |
| `tests/fixtures/golden/*` | CREATE | 黄金字节流 .bin + .expected.json |
| `scripts/record_golden.py` | CREATE | 字节流录制工具 |
| `cfg/servers.json` | CREATE | 服务器列表（从 opentdx const.py 提取） |

---

## NOT Building（Phase 1 范围外）

- **扩展行情**（期货/港美股，0x23f4-0x2489）→ Phase 2
- **SP/MAC 高级行情**（板块/资金流）→ Phase 2
- **数据管理层**（TdxData 统一API/Sync/Hybrid/复权/重采样/Calendar）→ Phase 3
- **并发批量/断点续传/落盘存储** → Phase 4 (v2)
- **HTTP 爬虫**（同花顺复权/gitee 财务）→ Won't
- **Python 绑定** → Won't

---

## Step-by-Step Tasks

依赖顺序执行。每个任务原子、可独立验证。**所有 fiber/Proactor 线程内代码强制遵守 fiber 纪律（PATTERN 4）。**

### Task 1: 项目骨架 + CMake + helio 集成
- **ACTION**: 创建 CMakeLists.txt + 目录结构 + helio submodule
- **IMPLEMENT**: `project(tdx-cpp VERSION 0.1.0)`；`set(CMAKE_CXX_STANDARD 17)`；`find_package(Boost CONFIG REQUIRED COMPONENTS context system)`；`set(CMAKE_MODULE_PATH ${helio}/cmake)`；`add_subdirectory(third_party/helio)`；定义 targets `tdx_util`/`tdx_proto`/`tdx_quotes`/`tdx_cli` + GoogleTest；目录 docs/cfg/src/scripts/tests/output
- **MIRROR**: dragonfly 顶层 `CMakeLists.txt:57,115-119`（helio 集成范式）
- **GOTCHA**: helio 无 install target，必须 add_subdirectory 内嵌；Boost 必须**系统装**（helio 不替你拉）；helio 自动拉 abseil/glog/liburing 等
- **VALIDATE**: `cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release && cmake --build build -j$(nproc)` —— 空骨架编译通过

### Task 2: 基础数据结构 + 协议常量 + 工具
- **ACTION**: 创建 `include/tdx/types.hpp`、`include/tdx/consts.hpp`、`src/util/{gbk,zlib,byte_order}.cpp`
- **IMPLEMENT**: 
  - `KLine`/`Quote`/`Tick`/`Xdxr` 结构体（**datetime 统一 int64 epoch seconds**；`Quote.bid/ask` 用 `std::array<double,5>`，未填档位 sentinel=NaN）
  - `MARKET`(SH=1/SZ=0/BJ/BJ)、`PERIOD`(0=5m..11=年)、`ADJUST`(NONE/QFQ/HFQ) 枚举
  - iconv GBK→UTF-8 封装、zlib 解压封装、小端字节序工具
- **MIRROR**: `opentdx/const.py:4-193`（枚举值 + 服务器列表）、PRD 数据结构定义
- **GOTCHA**: 服务器列表从 opentdx const.py 提取到 `cfg/servers.json`；iconv 用系统库（`libiconv`），GB18030 容错 GBK
- **VALIDATE**: 编译 + 单元测试（枚举值与上游一致、iconv 中文解码、zlib 解压）

### Task 3: 帧编解码（Frame）
- **ACTION**: 创建 `src/proto/frame.{hpp,cpp}`
- **IMPLEMENT**: 
  - 请求封包：`pack_request(head, customize, msg_id, payload)` → `<BIBHH`(head,customize,1,len,len) + `<H`(msg_id) + payload，对齐 `baseParser.py:9-25`
  - 响应解包：`unpack_response(head_buf[16])` → 校验 prefix=`b1 cb 74 00`（否则抛 TdxProtocolError）、解析 zipped/customize/msg_id/zipsize/unzip_size；`zipsize!=unzip_size` 时 zlib 解压 body，对齐 `baseStockClient.py:307-319`
- **MIRROR**: opentdx `baseParser.py:9-25` + `baseStockClient.py:286-321`
- **GOTCHA**: 全小端；prefix 校验失败必须抛异常不静默；按 zipsize 循环 recv 直到收满（对齐 :312-317）
- **VALIDATE**: 黄金字节流测试（帧层 D7/D8，见黄金字节流方案 §5.1）—— 压缩/未压缩响应、prefix 错误抛异常、zlib 损坏抛异常

### Task 4: 变长解码器（codec）
- **ACTION**: 创建 `src/proto/codec.{hpp,cpp}`（get_price + to_datetime）
- **IMPLEMENT**: 
  - `get_price(data, pos) → (int64, new_pos)`：bit0x40=符号、bit0x80=继续、首字节低6位、后续每字节低7位左移（pos_byte 从6起 +7），对齐 `help.py:137-169`
  - `to_datetime(num, with_time) → int64 epoch`：日K=YYYYMMDD；分钟线低16位=(year-2004)<<11|mmdd、高16位=当日分钟；**含 fallback**（越界互转），对齐 `help.py:171-207`
- **MIRROR**: opentdx `help.py:137-207`
- **GOTCHA**: get_price 越界返回 (0, pos+1)；to_datetime 的 fallback 分支必须与上游选择一致（黄金字节流方案 §6.2）
- **VALIDATE**: 专项测试（黄金字节流 D4/D5）—— 正/负/单/多字节 get_price、YYYYMMDD/紧凑/越界 fallback to_datetime，全边界对齐上游

### Task 5: Connection（helio FiberSocket 封装）
- **ACTION**: 创建 `src/proto/connection.{hpp,cpp}`
- **IMPLEMENT**: `Connection` 类封装 `FiberSocketBase`：`Connect(ep)`、`Send(bytes)`、`Recv(n)`、`Close()`、`IsConnClosed(ec)`；内部实现 `call(parser)` = `send(parser.serialize()) → recv_response() → parser.deserialize()`，对齐 opentdx `call`/`_send`
- **MIRROR**: PATTERN 1（echo_server.cc Driver）+ PATTERN 3（opentdx 收发骨架）
- **GOTCHA**: fiber 纪律（PATTERN 4）；`native_handle()` 可 setsockopt 调缓冲区（echo_server.cc:461-464）；连接断开用 `IsConnClosed` 判定
- **VALIDATE**: 集成测试（连真服前的 mock）—— 封包发送→收响应头→收 body→解压流程正确

### Task 6: RetryPolicy + CircuitBreaker（评审前移 P0-1）
- **ACTION**: 创建 `src/proto/retry.{hpp,cpp}`
- **IMPLEMENT**: 
  - `RetryPolicy`：指数退避序列 [0.1, 0.5, 1, 2]s ×4，成功即止，耗尽抛异常，对齐 `tdxdata/errors.py:42` + opentdx `baseStockClient.py:78-87`
  - `CircuitBreaker`：状态机 CLOSED→OPEN（5次失败）→HALF_OPEN（60s）→CLOSED/OPEN，用 `util::fb2::Mutex` 保护（**fiber 纪律**），对齐 `tdxdata/errors.py:90`
- **MIRROR**: tdxdata `errors.py:42,90`；opentdx `DefaultRetryStrategy`
- **GOTCHA**: CircuitBreaker 必须用 `util::fb2::Mutex`（非 std::mutex），否则 Proactor 卡死；退避用 `ThisFiber::SleepFor`（非 std::this_thread::sleep_for）
- **VALIDATE**: 状态机测试 —— Retry 退避序列、CircuitBreaker CLOSED/OPEN/HALF_OPEN 全转换

### Task 7: Heartbeat + ServerPool（自动选服）
- **ACTION**: 创建 `src/proto/heartbeat.{hpp,cpp}` + `src/proto/server_pool.{hpp,cpp}`
- **IMPLEMENT**: 
  - `Heartbeat`：`ProactorBase::AddPeriodic(15000, [this]{ send_heartbeat(); })`，20 次无业务断开，业务请求重置计数，对齐 `heartbeat.py:10-46`
  - `ServerPool`：`MakeFiber` 起 N fiber 并发测 hosts TCP 延迟（PATTERN 2），`Join` 收集选最优；**config.json 缓存**（首次测速落盘 `~/.tdx-cpp/config.json`，后续启动直读）
- **MIRROR**: PATTERN 2（echo_server.cc TLocalClient）+ opentdx `baseStockClient.py:138-177` + mootdx `server.py:42-71`、`config.py:15-52`
- **GOTCHA**: `AddPeriodic` 必须从该 Proactor 自己的线程调用；config.json 并发写需串行化（黄金字节流 fiber 时序测试）；IPv4/IPv6 自动（`:` 判定）
- **VALIDATE**: fiber 时序测试 —— 心跳 20 次断连（时间快进手段）、选服并发完成、config.json 并发写无损坏

### Task 8: ParserRegistry + 核心 Parser
- **ACTION**: 创建 `src/proto/parser_registry.{hpp,cpp}` + `src/proto/parsers/*`
- **IMPLEMENT**: 
  - `ParserRegistry`：msg_id→Parser 注册表（编译期或运行时 map）
  - Parser（每个含 serialize 请求体 + deserialize 响应）：
    - `0x0d Login`（`<B`=1）、`0x04 HeartBeat`
    - `0x523 K_Line`（请求 `<H6sHHHHH8s`；响应：count + 变长 OHLC[open/close/high/low 顺序!] + vol/amount + 可选 upCount/downCount 智能检测），对齐 `kline.py`
    - `0x537 TickChart`（请求 `<H6sHH`；响应：price/avg/vol **增量**累加），对齐 `tick_chart.py`
    - `0xfc5 Transaction`（请求 `<H6sHH`；响应：minutes + price/vol/trans/buy_sell/unknown 变长，**price 增量**），对齐 `transaction.py`
    - `0x53e QuotesDetail`（请求 `<H6sH`+N×`<B6s`；响应：price 基准 + OHLC/buy_sell 相对增量 + 5档相对增量），对齐 `quotes_detail.py`
    - `0x44d List`（请求 `<H3I`；响应：count + 37B/条 `<6sH16sfBfHH`），对齐 `list.py`
    - `0x44e Count`、`0x0f XDXR`（字段实现时从 server.py/client 确认）、`0x10 Finance`、F10
- **MIRROR**: opentdx 各 parser 文件（黄金字节流方案 §4/§5 逐字段）
- **GOTCHA**: K线 OHLC 顺序是 open/**close**/high/low（D3）；逐笔/分时 price 增量累加（D1）；报价 OHLC+五档相对 price 增量（D2）；upCount 智能检测（D6）—— 全部由黄金字节流测试覆盖
- **VALIDATE**: 黄金字节流测试（每个 msg_id，见黄金字节流方案 §5）—— C++ 解析结果与上游 expected.json 逐字段一致（浮点相对误差<1e-6）

### Task 9: VipdocReader（本地文件）
- **ACTION**: 创建 `src/proto/vipdoc_reader.{hpp,cpp}`
- **IMPLEMENT**: 读 `.day`(`<IffffIIf`)、`.lc1`(`<HHfffffII`)、`.lc5`(`<HHIIIIfII`)；路径 `{TDX_HOME}/vipdoc/{sh|sz|bj}/{lday|fzline|minline}/{code}.{day|lc5|lc1}`
- **MIRROR**: opentdx `reader/daily_bar_reader.py:35`、`min_bar_reader.py`
- **GOTCHA**: 88**** 板块指数放 sh 目录；本地读取**不在 fiber 内**（文件 IO，可用同步），但仍避免 std::mutex
- **VALIDATE**: local 测试 —— 读真实 `.day`/`.lc1`/`.lc5`，OHLCV 与通达信一致

### Task 10: StdQuotes（A股行情 API）
- **ACTION**: 创建 `src/quotes/std_quotes.{hpp,cpp}`
- **IMPLEMENT**: `StdQuotes` 类组合 Connection+Heartbeat+ServerPool+RetryPolicy+CircuitBreaker+ParserRegistry；方法 `bars(code,freq,start,offset)`/`quotes(code)`/`transaction(code,start,offset)`/`stocks(market)`/`xdxr(code)`/`finance(code)` 等，对齐 mootdx StdQuotes 签名
- **MIRROR**: mootdx `quotes.py:145-682`（StdQuotes 方法签名）、opentdx `quotationClient.py`
- **GOTCHA**: offset 上限 800（K线）/2000（分笔）；价格 /100、金额 *100 缩放（`quotationClient.py:24-30`）；每次业务请求重置心跳计数
- **VALIDATE**: live 测试（间加延迟，受 CircuitBreaker 保护）—— 拉 600000 日K，最新一根数据合理

### Task 11: 最小可观测性 + 优雅关闭
- **ACTION**: 在 connection/heartbeat/server_pool 加结构化日志；提供 `Shutdown()` drain fibers
- **IMPLEMENT**: glog 结构化日志（连接/熔断状态变更/心跳超时/选服结果）；`Shutdown()` 顺序：停心跳 AddPeriodic → drain in-flight fibers → Close sockets → ProactorPool::Stop
- **MIRROR**: helio glog 用法（echo_server.cc `LOG(INFO)`/`VLOG`/`LOG_IF`）；dragonfly `protocol_client.h` 生命周期
- **GOTCHA**: fiber 纪律；优雅关闭须先停周期任务再 drain，避免新请求涌入
- **VALIDATE**: 集成测试 —— 触发熔断/心跳超时产生日志；Shutdown 不泄漏 fiber/socket

### Task 12: 黄金字节流录制工具 + fixtures
- **ACTION**: 创建 `scripts/record_golden.py` + `tests/fixtures/golden/*`
- **IMPLEMENT**: 录制工具对 P0 msg_id × 代表股票（600000 沪/000001 深）连真服，抓响应字节 + 调 opentdx 解析，生成 `.bin` + `.expected.json`；入库 git
- **MIRROR**: 黄金字节流方案 §3（方法论）、§7（录制维护流程）
- **GOTCHA**: 录制时间服加延迟避免限流；行情数据非敏感可入库；录制脚本不硬编码敏感配置
- **VALIDATE**: P0 msg_id golden fixtures 就位，C++ 解析测试全过

### Task 13: CLI 骨架
- **ACTION**: 创建 `src/cli/main.cpp`
- **IMPLEMENT**: `tdx` 命令 + 子命令 `server-test`（测速）、`bars -s <code> -p <period> -n <count>`（拉K线打印）。链接 tdx_quotes
- **MIRROR**: mootdx CLI（`__main__.py`）
- **GOTCHA**: 精度格式遵循全局规范（价位 %.2f、数量 %d）；CLI 在 main 线程，不在 fiber 内
- **VALIDATE**: `./build/tdx server-test` 输出测速结果；`./build/tdx bars -s 600000 -p 9 -n 10` 输出 10 根日K

---

## Testing Strategy

### 单元测试（对齐 opentdx 332 + tdxdata 172）
| 测试文件 | 用例 | 验证 |
|---|---|---|
| `tests/test_frame.cpp` | 封包/解包/prefix校验/zlib | D7/D8 |
| `tests/test_codec.cpp` | get_price/to_datetime 全边界 | D4/D5 |
| `tests/test_retry.cpp` | Retry退避序列、CircuitBreaker状态机 | 弹性层 |
| `tests/test_parsers.cpp` | 各 msg_id 黄金字节流解析 | D1/D2/D3/D6 |
| `tests/test_vipdoc_reader.cpp` | .day/.lc1/.lc5 解析 | 本地文件 |
| `tests/test_consts.cpp` | 枚举值与上游一致 | 协议常量 |

### 黄金字节流测试（Success Signal ③ 核心）
- 详见 `phase1-golden-byte-stream.md`。P0 msg_id：帧层/0x523/0xfc5/0x537/0x53e/0x44d。C++ 输出 vs `.expected.json` 逐字段 deep equal（浮点相对误差<1e-6）。

### helio fiber 异步时序测试
| 用例 | 验证 |
|---|---|
| fiber 内异常传播 | 崩 ProactorPool 还是单 fiber（行为明确） |
| 心跳 20 次断连时序 | AddPeriodic + 时间快进 |
| 选服并发 + config.json 并发写 | MakeFiber 竞态无损坏 |

### local 测试（对齐 tdxdata 20）
- 读真实 vipdoc `.day/.lc1/.lc5`，OHLCV 与通达信一致。

### live 测试（对齐 tdxdata 44，可跳过）
- **间加延迟/退避 + CircuitBreaker 保护**；连真服拉 600000 日K/五档/逐笔；返回>50 条验样本/总数。

### Edge Cases Checklist
- [ ] count=0 响应（空结果不崩溃）
- [ ] 响应截断（pos+len>data_len 安全停止）
- [ ] prefix 错误/zlib 损坏（抛异常不静默）
- [ ] get_price 越界（返回 0, pos+1）
- [ ] 盘前五档部分缺失（sentinel 处理）
- [ ] 中文股票名 GBK 解码
- [ ] 连接断开（IsConnClosed + 重连）

---

## Validation Commands

### Level 1: 静态分析
```bash
cmake --build build -j$(nproc) --target tidy  # clang-tidy（含 fiber 纪律扫描）
```
**EXPECT**: Exit 0，无禁用符号（std::mutex/sleep_for 在 fiber 代码内）

### Level 2: 单元测试
```bash
cmake --build build -j$(nproc)
ctest --test-dir build -j$(nproc) --output-on-failure
ctest --test-dir build -R golden -V   # 黄金字节流专项
```
**EXPECT**: 全过，覆盖率 >80%（gcov/lcov）

### Level 3: 全套
```bash
cmake --build build -j$(nproc) && ctest --test-dir build -j$(nproc)
```
**EXPECT**: 编译 + 全测试通过

### Level 4: live 集成（手动/可选）
```bash
./build/tdx server-test                    # 测速
./build/tdx bars -s 600000 -p 9 -n 10      # 拉日K
ctest --test-dir build -L live -V          # live 测试（间加延迟）
```
**EXPECT**: 真服数据合理，与上游数据点一致

---

## Acceptance Criteria（对齐 PRD Phase 1 Success Signal）

- [ ] ① live 测试（间加延迟，受 CircuitBreaker 保护）连真服拉到 600000 真实日K，与上游数据点一致
- [ ] ② local 测试解析真实 vipdoc `.day` 文件输出正确
- [ ] ③ 黄金字节流单测全过，覆盖 msg_id（帧层/0x523/0xfc5/0x537/0x53e/0x44d/0x0d/0x04），见 `phase1-golden-byte-stream.md`
- [ ] ④ CLAUDE.md fiber 纪律审查无违规（clang-tidy 扫描通过）
- [ ] ⑤ RetryPolicy/CircuitBreaker 状态机测试通过
- [ ] ⑥ 覆盖率 >80%
- [ ] 代码镜像 helio/opentdx 模式（fiber 用法、收发骨架、字段格式逐字对齐）
- [ ] 无现有测试回归（无现有代码，N/A）

---

## Completion Checklist

- [ ] 13 个任务按依赖顺序完成
- [ ] 每个任务完成后立即验证（VALIDATE 命令）
- [ ] Level 1-3 验证全过
- [ ] Level 4 live 验证（可选，间加延迟）
- [ ] 所有 Acceptance Criteria 满足
- [ ] 黄金字节流 fixtures 入库

---

## Risks and Mitigations

| 风险 | 可能性 | 影响 | 缓解 |
|---|---|---|---|
| 变长/增量编码移植错误 | 高 | 数据全错 | 黄金字节流逐字段对齐（Task 4/8/12） |
| helio fiber 纪律违反 | 中 | Proactor 卡死 | CLAUDE.md 规范 + clang-tidy 扫描（Task 1 配置） |
| io_uring 内核<5.11 | 中 | 回退 epoll | 文档标注；CI 双测 |
| Boost 系统依赖缺失 | 低 | 构建失败 | README 前置依赖 + 安装脚本 |
| 通达信限流 | 中 | live 测试不稳 | CircuitBreaker + 延迟退避；mock 优先 |
| 0x0f xdxr 字段待定位 | 中 | Task 8 阻塞 | 实现时从 opentdx server.py/client 确认 |
| helio 集成体积大 | 低 | 部署 | 已知 trade-off，Success Metric 已澄清 |

---

## Notes

- **依赖关键路径**：Task 1（骨架）→ Task 2（结构体/工具）→ [Task 3 Frame ‖ Task 4 codec] → Task 5 Connection → [Task 6 Retry ‖ Task 7 Heartbeat/ServerPool] → Task 8 Parser → Task 10 StdQuotes → Task 13 CLI。Task 9/11/12 可与后期并行。
- **黄金字节流是正确性地基**：Task 12 的录制工具 + fixtures 应在 Task 8 实现中期就位，边实现边对齐，不要等到最后。
- **fiber 纪律是隐形陷阱**：Task 1 即配置 clang-tidy 扫描禁用符号，每个 fiber 内代码任务（5/6/7/8/10）PR 审查必检。
- **版本号**：CMakeLists.txt `project(tdx-cpp VERSION 0.1.0)`，遵循全局规范打 tag（Phase 1 完成可打 0.1.0）。

---

*下一步：评审本计划 → 运行 `/prp-implement .claude/PRPs/plans/phase1-protocol-layer-a-share.plan.md` 执行 Phase 1。*
