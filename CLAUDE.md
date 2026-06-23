# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## 项目状态

**Phase 1-4 全部完成（v0.5.0）。** 协议层 + A股标准行情（Phase 1, v0.1.0）+ 扩展行情/SP/MAC（Phase 2, v0.2.0）+ 数据管理核心 Calendar/Adjust/Resampler/SyncState/TdxData（Phase 3, v0.3.0）+ v2 改进 external/统一/DuckDB/断点续传/并发批量（Phase 4, v0.4.0）+ 加固 time_util abseil 替代 POSIX / SQL 注入防护 / 离线构建 / e2e 真网测试（v0.5.0）。本文件作为 C++ 实现的设计蓝图与协议知识库。

## 项目目标

用 C++ 单库实现通达信（TDX）行情数据的读取，功能范围对齐以下三个 Python 项目（位于父目录）：

| 上游项目 | 路径 | 在 C++ 版中的对应层 | 核心职责 |
|---|---|---|---|
| **opentdx** | `/home/li/peiking88/opentdx` | 协议层（最底层，最先移植） | TCP socket、二进制帧编解码、命令号注册表、本地 `.day/.lc1/.lc5` 读取、服务器列表与并发测速 |
| **mootdx** | `/home/li/peiking88/mootdx` | 行情接口层 | 工厂分发（标准/扩展市场）、语义化 API、财务下载解析、复权因子、板块解析 |
| **tdxdata** | `/home/li/peiking88/tdxdata` | 数据管理层（最上层） | 统一 `TdxData` API、熔断器+重试、增量同步、本地优先+网络补缺的混合数据源、自算复权、A 股时段感知重采样 |

依赖链上游是 `tdxdata → mootdx → opentdx`。C++ 版应将三层合并为一个库，分层但同源，不再跨语言依赖。

## 通达信协议核心知识（移植时必须逐字对照）

这是 C++ 重写最大的工作量与风险点，全部源自上游实测：

### 连接
- **传输层**：原生 TCP socket（非 HTTP），默认 `time_out=5` 秒，IPv4/IPv6 自动选择。参考 `opentdx/client/baseStockClient.py:138-227`。
- **端口**：标准行情 `7709`；扩展行情（期货/港美股）`7727`；SP/MAC 高级行情（板块/资金流）走 `mac_hosts` 也用 `7709`。参考 `opentdx/const.py:4-193`。
- **登录**：连接后必须发送 `Login`（msg_id `0x0d`），请求体 `struct.pack('<B', 1)`。SP 模式登录是不同的 msg_id `0x2454`。参考 `opentdx/parser/server.py`。
- **心跳**：`HeartBeatThread`，**15 秒间隔**（`DEFAULT_HEARTBEAT_INTERVAL=15.0`），连续 20 次纯心跳无业务请求则主动断开。每次业务请求必须重置该计数。参考 `opentdx/utils/heartbeat.py:10-46`。
- **自动选服**：未指定 IP 时并发（上游用 `ThreadPoolExecutor` max 10）测所有 host 的 TCP 连接延迟，选最快者。参考 `opentdx/client/baseStockClient.py:138-177`、`mootdx/server.py:42-71`。
- **自动重试**：上游策略 `[0.1, 0.5, 1, 2]` 秒共 4 次，每次重连重发。参考 `opentdx/client/baseStockClient.py:78-87`。

### 二进制帧格式（全小端 `<`）
- **请求头 12 字节**：`struct.pack('<BIBHH', head, customize, control=1, lbody, lbody)`，紧跟 body = `struct.pack('<H', msg_id) + payload`。`head`：`0x0c`=不压缩 / `0x1c`=请求压缩。参考 `opentdx/parser/baseParser.py:9-25`。
- **响应头 16 字节**（`RSP_HEADER_LEN=0x10`）：`struct.unpack('<IBIBHHH', ...)` → prefix(4B，固定 `b1 cb 74 00`) / zipped / customize / unknown / msg_id / zipsize / unzip_size。当 `zipsize != unzip_size` 时 body 用 **zlib 解压**（注意：是 zlib，不是 lzma）。参考 `opentdx/client/baseStockClient.py:283-321`。
- **中文编码**：**GBK** 解码（如股票名称、公告）。
- **价格缩放**：实时行情价格需 `/100`，金额 `*100`；财务字段常以「万元」为单位需 `*10000` 还原。参考 `opentdx/client/quotationClient.py:24-30`、`mootdx/hq_adapter.py:293-294`。

### 非平凡编码（务必仔细移植，易错）
- **变长价格 `get_price`**：类 UTF-8 变长有符号整数——`bit 0x40`=符号位、`bit 0x80`=继续位、每后续字节 7 bit。参考 `opentdx/utils/help.py:137-169`。
- **紧凑日期 `to_datetime`**：日 K 用 `YYYYMMDD` 整数；分钟线用紧凑编码（低 16 位 = `(year-2004)<<11 | month*100+day`，高 16 位 = 当日分钟数）。参考 `opentdx/utils/help.py:171-207`。
- **K 线请求体**：`struct.pack('<H6sHHHHH8s', market, code.gbk, period, times, start, count, adjust, b'')`——注意 code 是 **6 字节 GBK**。参考 `opentdx/parser/quotation/kline.py:12`。

### 命令号（msg_id）分区
C++ 用注册表（`msg_id → 解析器`）实现。上游用 `@register_parser(msg_id, head, customize, need_zip)` 装饰器，见 `opentdx/parser/`。主要分区：
- **A股行情**（`head=0x0c, customize=0`）：`0x04` 心跳、`0x0d` 登录、`0x0f` 除权除息、`0x10` 财务、`0x44d` 列表、`0x44e` 数量、`0x523` K线（核心）、`0x537` 分时、`0x53e` 详细报价、`0x53f` 排行榜、`0x563` 主力异动、`0x56a` 集合竞价、`0xfc5` 实时逐笔、`0xfeb` 历史分时、`0xfb4/0xfb5` 历史委托/成交、`0x2cf/0x2d0` F10。
- **扩展行情**（`customize=1`）：`0x23f4/0x23f5` 类别/列表、`0x23ff/0x2489` K线、`0x23fa` 单报价、`0x2412` 历史成交、`0x2454` 登录。
- **SP/MAC 协议**（`customize=1 或 2`）：`0x1218` 资金流向、`0x122C` 板块成员报价、`0x122E` 板块K线、`0x1231` 板块列表、`0x1237` 异动、`0x123D` 竞价。SP 字段用位图映射，见 `opentdx/utils/bitmap.py`。

### 本地 vipdoc 文件格式
读取通达信本地目录（环境变量 `TDX_HOME` 或默认安装路径）：
- `.day` 日线：`<IffffIIf`（扩展行情多一列 hk_stock_amount）。参考 `opentdx/reader/daily_bar_reader.py:35`。
- `.lc1` 1分钟线：`<HHfffffII`；`.lc5` 5分钟线：`<HHIIIIfII`。参考 `opentdx/reader/min_bar_reader.py`、`lc_min_bar_reader.py`。
- 路径模式：`{tdxdir}/vipdoc/{sh|sz|bj|ds}/{lday|fzline|minline}/{symbol}.{day|lc5|lc1|5|1}`。`88****` 板块指数放 `sh` 目录；`#` 开头为扩展市场 `ds`。
- 板块文件 `block.dat` 等固定 2800 字节/块。参考 `opentdx/utils/block_reader.py:40-70`。

## 行情接口范围（功能对齐清单）

- **标准行情（A股，端口 7709）**：K线（含前/后复权）、分时图、逐笔成交、五档报价、股票列表/数量、除权除息、财务、F10、指数、成交量分布。
- **扩展行情（期货/港美股/期权/债券，端口 7727）**：商品列表、报价、K线、历史成交、分时。
- **SP/MAC 高级行情**：板块列表/成员、资金流向、集合竞价、异动监控（需 `client.sp()` 切换到 mac_hosts）。
- **本地数据**：`.day/.lc1/.lc5` 读取与时效性校验。
- **数据管理（tdxdata 层）**：统一 API、熔断器+指数退避重试、增量同步（JSON 状态持久化）、本地优先+网络补缺的混合数据源、自算复权因子、A 股交易时段感知的 K 线重采样、标准化输出 schema。

## 上游短板与改进路线（Phase 4 全部完成 ✅）

上游 Python 版有三个明确缺陷，已在 Phase 4（v0.4.0）全部解决：

1. **并发批量下载**（已完成）：`tdx_batch` 模块——helio fiber 池 + 全局规范要求的 `-n` 参数。见 `src/batch/batch_fetch.cpp`。
2. **断点续传**（已完成）：`tdx_data::SyncState` 实现股票级进度持久化（JSON），支持崩溃恢复。见 `src/data/sync_state.cpp`。
3. **存储后端**（已完成）：`tdx_query::DuckDBQuery`——DuckDB 嵌入式 SQL 引擎（进程内，零外部服务），Parquet 读写（COPY TO / SELECT FROM）+ 即席 SQL 查询 + 内存热表（最新报价/订阅状态），**无 Arrow 依赖**。第三方依赖统一收纳 `external/`（helio rsync 复制 + duckdb vendored + abseil 等预下载）。

## 技术栈与目录结构（架构评审已确认，见 `.claude/PRPs/prds/tdx-cpp.prd.md`）

- **构建**：CMake + Ninja（两者均已安装）。**C++ 标准定为 C++17**。
- **版本号**：写在 `CMakeLists.txt` 的 `project(tdx-cpp VERSION x.y.z)`，遵循全局规范的打 tag 规则（不加 `v` 前缀）。
- **异步 IO**：**helio**（`external/helio`，由 `scripts/setup_external.sh` 从 `~/framework/dragonfly/helio` rsync 复制源码（排除 build/.git/），io_uring+epoll+自研 fiber 协程，C++17）。`AddPeriodic` 做 15s 心跳、`MakeFiber` 做并发测速、`FiberSocketBase` 做 TCP。参考 `helio/examples/echo_server.cc` 的 `Driver`/`TLocalClient`。**纪律：禁用 `std::mutex`/`std::thread::sleep_for`，须用 `util::fb2::Mutex`/`ThisFiber::SleepFor`**，否则整个 Proactor 线程卡死。无 install target，须 `add_subdirectory(external/helio)` 内嵌；需系统 Boost(context+system)，其余依赖(abseil/glog/liburing 等)自动拉。
- **GBK 转码**：系统 iconv。
- **压缩**：zlib（协议必需）。
- **测试**：GoogleTest + CTest，live 测试间加延迟退避避免限流。
- **v2 存储**：**DuckDB 嵌入式**（Parquet 读写 COPY TO/SELECT FROM + SQL 查询 + 内存热表，**无 Arrow**），vendored `external/duckdb`（libduckdb.so + duckdb.hpp，镜像下载，CMake IMPORTED target）。
- **目录结构**：
  ```
  docs/        API 文档、协议说明、PRD（规划中，当前为空）
  cfg/         配置文件（servers.json / holidays.json）
  include/     公共头文件（tdx/ 命名空间）
    ├─ tdx/types.hpp           KLine/Tick/Transaction/Quote/Stock 结构体
    ├─ tdx/consts.hpp          协议常量、市场/周期/交易时段枚举
    ├─ tdx/errors.hpp          异常类型
    ├─ tdx/proto/              协议层头文件（13 个）
    ├─ tdx/quotes/             行情接口层头文件（3 个）
    ├─ tdx/data/               数据管理层头文件（5 个）
    ├─ tdx/query/              DuckDB 查询层头文件（1 个）
    ├─ tdx/batch/              批量拉取头文件（1 个）
    ├─ tdx/util/               工具头文件（4 个：gbk/zlib/time_util/byte_order）
    └─ nlohmann/               JSON 库（vendored 单头文件）
  src/         源码
    ├─ util/                   gbk / zlib / time_util（absl::Time+FixedTimeZone）
    ├─ proto/                  协议层（14 文件，拆分为 4 子 target：core/transport/parsers/local）
    ├─ quotes/                 行情接口层（3 文件：std/ext/sp）
    ├─ data/                   数据管理层（5 文件）
    ├─ query/                  DuckDB 查询层（1 文件）
    ├─ batch/                  并发批量拉取（1 文件）
    └─ cli/                    CLI 入口
  scripts/     辅助脚本（setup_external.sh / record_golden.py）
  tests/       单元测试 + 集成测试（15 文件）
    └─ fixtures/golden/        黄金字节流（真服录制）
  external/    第三方依赖（不入 git，setup_external.sh 初始化）
    ├─ helio/                  io_uring + fiber 框架（rsync 复制）
    ├─ duckdb/                 DuckDB vendored（libduckdb.so + duckdb.hpp）
    ├─ googletest/             预下载 tarball（离线构建用）
    ├─ abseil/                 预下载 tarball
    ├─ benchmark/              预下载 tarball
    └─ ...                     gperf/xxhash/uring/pugixml/cares/zstd/rapidjson/expected
  output/      程序输出（不入 git）
  ```

## 命名空间与模块架构

| 命名空间 | 对应 CMake target | 职责 |
|---|---|---|
| `tdx::util` | `tdx_util` | 工具（GBK iconv、zlib 解压、**absl::Time+FixedTimeZone(+8)** 替代 POSIX timegm/gmtime_r、字节序转换） |
| `tdx::proto` | `tdx_proto`（umbrella INTERFACE） | 协议层：帧编解码（core）、连接/心跳/熔断/选服（transport）、解析器（parsers）、本地文件（local） |
| `tdx::quotes` | `tdx_quotes` | 行情接口层（StdQuotes / ExtQuotes / SpQuotes） |
| `tdx::data` | `tdx_data` | 数据管理层（Calendar / Adjust / Resampler / SyncState / TdxData） |
| `tdx::query` | `tdx_query` | DuckDB 查询层（Parquet 读写 + 内存热表 + SQL 注入防护） |
| `tdx::batch` | `tdx_batch` | 并发批量拉取（helio fiber 池分片 + `-n` 并发数） |
| `tdx` (exe) | `tdx` | CLI 入口（server-test / bars / ex-bars / fetch-history） |

## helio fiber 编码纪律（关键约束）

helio 的 Proactor 线程内**禁用**标准库阻塞原语——它们会阻塞整个事件循环线程（不只当前 fiber）：

| 禁用 | 替代（`util::fb2::`） |
|---|---|
| `std::mutex` / `std::lock_guard` | `util::fb2::Mutex` |
| `std::condition_variable` | `util::fb2::CondVar` / `EventCount` / `Done` |
| `std::this_thread::sleep_for` | `ThisFiber::SleepFor` |

来源：`helio/CLAUDE.md:67-77`。

**适用范围**：所有在 helio Proactor 线程 / fiber 上下文内运行的代码——主要是 `tdx::proto` 协议层全部，以及任何被 fiber 调用的逻辑。写这些代码时必须应用本纪律。

**测试代码豁免**（评审决策 #3）：gtest 测试框架与测试编排运行在 Proactor 线程之外（main / gtest worker 线程），**豁免**此纪律。但：① 被测的 fiber 内业务代码仍须遵守；② 测试若需在 Proactor 线程内编排（如等待 fiber 完成），用 `ProactorPool::Await` / `Fiber::Join`，不要裸 `std::this_thread::sleep_for`。

**第三方库审计**：在 fiber 内调用第三方库前，确认其内部无阻塞原语——iconv / zlib 安全；nlohmann_json / spdlog / fmt 需确认，或避免在 fiber 内调用（改在 fiber 外做 I/O，或用 fb2 同步原语保护）。

**工程手段**：CI clang-tidy 扫描禁用符号为基线；头文件 `#define` / `static_assert` 编译期拦截为可选增强（评审选择规范为主、按需应用）。

## 构建、测试、运行命令（建议脚手架）

```bash
# 0. 首次初始化 external/ 依赖（helio 源码 + DuckDB + 预下载 tarball，支持离线构建）
bash scripts/setup_external.sh

# 1. 配置（Release，ninja，并行）
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
# 2. 编译（全局规范：-j$(nproc) 并行）
cmake --build build -j$(nproc)

# 3. 运行全部测试（ctest 并行）
ctest --test-dir build -j$(nproc) --output-on-failure
# 运行单个测试用例
ctest --test-dir build -R <test_name> -V

# 4. CLI 示例
./build/bin/tdx server-test                              # 测速选服
./build/bin/tdx bars 600000 4 10                         # 拉日 K
./build/bin/tdx ex-bars 31 HSImain 4 10                  # 扩展行情 K 线
./build/bin/tdx fetch-history --stock-list stock.txt \
    --start 2024-01-01 --end 2024-06-01 -n 16 --resume   # 批量拉取+断点续传
```

**真实网络测试**（连接通达信服务器）优先级高于 mock，参考上游的 `pytest -m live`（连真服）与 `-m local`（读本地文件）划分。e2e 测试（`test_e2e`）在服务器不可达时自动 `GTEST_SKIP`，不视为失败。

## 关键实现注意事项

- **精度**：遵循全局规范——价位/金额 `%.2f`、数量 `%d`、百分比 `%d%%`。
- **复权**：tdxdata 的复权因子是**基于 xdxr 事件流自行计算**（`tdxdata/sources/adjust.py:49`，区分前复权 qfq 的 backward-asof 与后复权 hfq 的 forward-asof），不是直接取交易所因子。移植时须对照其单测。
- **A 股时段感知重采样**：15m/30m/1h 由 5m 重采样，1w/1mon 由 1d 重采样，且 K 线结束时间须按 A 股交易时段（上午 9:30、下午 13:00 开盘）标注。参考 `tdxdata/sources/base.py:56-132`。
- **K 线周期常量**：`0`=5min、`1`=15min、`2`=30min、`3`=1h、`4`=日、`5`=周、`6`=月、`7`=扩展1min、`8`=1min、`9`=日K、`10`=季、`11`=年。单次请求 K 线上限 800 条、分笔 2000 条。
- **结构体移植**：所有 `struct.unpack` 格式串须逐字移植，不可臆测字节序。
- **时区转换（v0.5.0 重构）**：`time_util` 用 `absl::Time` + `absl::FixedTimeZone(+8)` 替代 POSIX `timegm`/`gmtime_r`——消除非标准依赖、天然线程安全、为跨时区扩展铺路。见 `src/util/time_util.cpp`。
- **SQL 注入防护（v0.5.0）**：`DuckDBQuery` 所有拼接 SQL 通过 `EscapeSql()` 转义单引号（`''`），含引号 code 可安全使用。见 `src/query/duckdb_query.cpp`。
- **离线构建（v0.5.0）**：`scripts/setup_external.sh` 预下载 googletest/benchmark/abseil-cpp/gperf/xxhash/liburing 等依赖的 tarball 到 `external/`，CMake FetchContent 使用 `file://` URL 离线使用，构建期无需联网。

## 参考资源

- 协议层实现样板：`/home/li/peiking88/opentdx/parser/baseParser.py`（封包）、`opentdx/client/baseStockClient.py:283-321`（收发+解压）。
- 全量命令号与服务器列表：`/home/li/peiking88/opentdx/const.py`。
- 统一 API 与熔断/增量设计：`/home/li/peiking88/tdxdata/api.py`、`tdxdata/core/data_manager.py`、`tdxdata/errors.py`、`tdxdata/sync.py`。
- 复权计算与时段重采样：`/home/li/peiking88/tdxdata/sources/adjust.py`、`base.py`。
- 用户全局开发规范：`~/.claude/CLAUDE.md`（Git 认证、中文、编译并行参数、测试覆盖率>80%、精度格式等，本项目一并遵循）。
