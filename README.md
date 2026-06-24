# tdx-cpp

通达信（TDX）行情数据 C++ 单库 v0.6.0。用 C++17 重写 [opentdx](https://github.com/rainx/opentdx)（协议层）+ [mootdx](https://github.com/mootdx/mootdx)（行情接口层）+ [tdxdata](https://github.com/rainx/tdxdata)（数据管理层），消除 Python 运行时依赖，获得确定性性能与协议字节级正确性。

异步 IO 采用 [helio](https://github.com/romange/helio)（io_uring + fiber 协程，C++17），存储采用 [DuckDB](https://duckdb.org/) 嵌入式引擎（Parquet 读写 + SQL 查询 + 内存热表，无 Arrow 依赖）。

## 当前状态：Phase 1-4 全部完成（v0.6.0）

| 能力 | 模块 | 状态 |
|---|---|---|
| 协议层（帧编解码 / 变长 codec / Connection / Heartbeat / ServerPool） | `tdx_proto_core` + `tdx_proto_transport` | ✅ |
| 熔断器 + 退避重试 | `tdx_proto_transport` | ✅ |
| A 股标准行情解析（K线/分时/逐笔/五档/列表/数量） | `tdx_proto_parsers` | ✅ |
| 扩展行情（期货/港美股/期权/债券） | `tdx_proto_parsers` | ✅ |
| SP/MAC 高级行情（板块/资金流/位图） | `tdx_proto_parsers` | ✅ |
| 本地 vipdoc 读取（.day/.lc1/.lc5/block.dat） | `tdx_proto_local` | ✅ |
| StdQuotes / ExtQuotes / SpQuotes 集成 API | `tdx_quotes` | ✅ |
| 自动选服（并发测速）+ 心跳 + 熔断保护 | `tdx_quotes` | ✅ |
| 数据管理（Calendar/Adjust/Resampler/SyncState/TdxData） | `tdx_data` | ✅ |
| 复权因子自算（qfq/hfq）+ A 股时段感知重采样 | `tdx_data` | ✅ |
| 并发批量拉取（helio fiber 分片，`-n` 并发数） | `tdx_batch` | ✅ |
| 股票级断点续传（JSON 状态持久化 + 崩溃恢复） | `tdx_data` | ✅ |
| DuckDB Parquet 落盘 + 即席 SQL | `tdx_query` | ✅ |
| CLI 工具（bars / ex-bars / fetch-history） | `tdx` | ✅ |
| 真网端到端集成测试 | `test_e2e` | ✅ |

## 构建

### 初始化（首次）

```bash
# 初始化 external/ 依赖（helio 源码 + DuckDB + 预下载 tarball）
bash scripts/setup_external.sh
```

### 配置与编译

```bash
# 配置（Release，ninja）
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release

# 编译全部
cmake --build build -j$(nproc)

# 仅编译 CLI 工具
cmake --build build --target tdx -j$(nproc)
```

helio 源码路径可用 `-DHELIO_PATH=<path>` 或环境变量 `HELIO_PATH` 配置（默认 `external/helio`）。

### 系统依赖

- C++17 编译器（GCC ≥ 9 或 Clang ≥ 10）
- CMake ≥ 3.16 + Ninja
- Boost（context + system，1.76+）
- ZLIB
- OpenSSL
- iconv（glibc 内置，Linux 无需额外安装）

`external/` 目录预置以下构建期依赖（无需联网）：

| 依赖 | 位置 | 说明 |
|---|---|---|
| helio | `external/helio/` | io_uring + fiber 协程框架 |
| DuckDB 1.1.3 | `external/duckdb/` | 嵌入式 SQL 引擎（vendored） |
| nlohmann/json | `include/nlohmann/` | JSON 解析（vendored） |
| googletest / benchmark / abseil-cpp | `external/` | 预下载 tarball，CMake FetchContent 离线使用 |
| gperf / xxhash / liburing / pugixml / c-ares / zstd | `external/` | ExternalProject 预置，构建期跳过下载 |
| rapidjson / expected-lite | `external/` | git bare clone，git insteadOf 本地使用 |

## 用法

```bash
# 测速标准行情服务器（自动选最快）
./build/bin/tdx server-test

# 拉取 K 线（代码 周期 数量）
# 周期：0=5分 4=日 5=周 6=月 7=1分 8=多分钟 9=多日 11=年
./build/bin/tdx bars 600000 4 10

# 拉取扩展行情 K 线
./build/bin/tdx ex-bars 31 HSImain 4 10

# 批量拉取历史 K 线（-n 并发数，--resume 断点续传）
./build/bin/tdx fetch-history --stock-list stock.txt --start 2024-01-01 --end 2024-06-01 -n 16 --resume
```

## 测试

### 运行全部测试

```bash
cmake --build build -j$(nproc)
ctest --test-dir build -j$(nproc) --output-on-failure
```

### 运行特定测试

```bash
# 单测
./build/bin/test_codec          # 变长编码 + 紧凑日期全边界
./build/bin/test_parsers        # K线/分时/逐笔/五档/列表解析 + 错误路径
./build/bin/test_duckdb         # Parquet 读写 + 内存热表 + SQL 注入防护
./build/bin/test_phase3         # Calendar/Adjust/Resampler + 午休/收盘边界
./build/bin/test_sync4          # 断点续传 + fiber 并发安全 + 损坏 JSON 降级

# 集成测试（需通达信网络可达；不可达自动跳过）
./build/bin/test_e2e            # TCP → 登录 → K线 → 五档 → DuckDB 落盘
./build/bin/test_golden         # 真服录制黄金字节流
./build/bin/test_retry          # 熔断器状态机 + 重试退避
```

### 测试覆盖率

15 个测试文件 → 38 个 CTest 用例（含 helio 23 个 CI 用例），全部通过。

```
test_codec          # codec 层（get_price / to_datetime）
test_frame          # 帧编解码
test_util           # GBK 转码 / byte_order
test_parsers        # 标准行情解析 + 错误路径
test_phase2         # 扩展行情 + SP 协议
test_sp_codec       # SP 编码
test_json           # JSON 解析
test_vipdoc_reader  # 本地 vipdoc 文件
test_golden         # 黄金字节流（真服录制）
test_phase3         # 数据层（Calendar/Adjust/Resampler）
test_sync4          # 断点续传 + 并发安全
test_duckdb         # DuckDB 查询层
test_retry          # 熔断器 + 重试退避
test_server_pool    # 并发测速选服
test_e2e            # 端到端集成测试（真网）
```

## 架构

### 依赖层次（自下而上）

```
                          tdx（CLI）
                            │
              ┌─────────────┼─────────────┐
              │             │             │
          tdx_batch     tdx_data      tdx_query
              │             │             │
              ├─────────────┤             │
              │         tdx_quotes ───────┘
              │             │
              └──────┬──────┘
                     │
                tdx_proto（INTERFACE umbrella）
          ┌──────────┼──────────┬──────────┐
          │          │          │          │
   tdx_proto_core  parsers   local    transport
   (frame/codec)   (7文件) (vipdoc) (conn/retry)
          │
       tdx_util（gbk/zlib-ng/absl::Time）
          │
    ┌─────┴──────────┐
   zlib-ng    absl::time
```

### 命名空间

| 命名空间 | 职责 |
|---|---|
| `tdx::proto` | 协议层（帧、编解码、连接、解析器） |
| `tdx::quotes` | 行情接口层（StdQuotes / ExtQuotes / SpQuotes） |
| `tdx::data` | 数据管理层（Calendar / Adjust / Resampler / SyncState / TdxData） |
| `tdx::query` | DuckDB 查询层 |
| `tdx::batch` | 并发批量拉取 |
| `tdx::util` | 工具（GBK iconv、zlib-ng 解压、absl::Time 时区、字节序） |

### helio fiber 纪律

协议层和行情接口层在 Proactor 线程/fiber 内执行，禁用 `std::mutex` / `std::this_thread::sleep_for`，统一用 `util::fb2::Mutex` / `ThisFiber::SleepFor`。测试代码豁免此纪律（gtest 运行在 Proactor 线程外）。详见 `CLAUDE.md`。

## 目录结构

```
docs/        API 文档、协议说明、PRD
cfg/         配置文件（servers.json / holidays.json）
include/     公共头文件（tdx/ 命名空间）
  ├─ tdx/types.hpp          KLine/Tick/Transaction/Quote/Stock 结构体
  ├─ tdx/consts.hpp         协议常量、市场/周期/交易时段枚举
  ├─ tdx/errors.hpp         异常类型
  ├─ tdx/proto/             协议层头文件（13 个）
  ├─ tdx/quotes/            行情接口层头文件（3 个）
  ├─ tdx/data/              数据管理层头文件（5 个）
  ├─ tdx/query/             DuckDB 查询层头文件（1 个）
  ├─ tdx/batch/             批量拉取头文件（1 个）
  ├─ tdx/util/              工具头文件（4 个：gbk/zlib/time_util/byte_order）
  └─ nlohmann/              JSON 库（vendored 单头文件）
src/         源码
  ├─ util/                  gbk / zlib / time_util（absl::Time）
  ├─ proto/                 协议层（14 文件，拆分为 4 子 target）
  ├─ quotes/                行情接口层（3 文件）
  ├─ data/                  数据管理层（5 文件）
  ├─ query/                 DuckDB 查询层（1 文件）
  ├─ batch/                 并发批量拉取（1 文件）
  └─ cli/                   CLI 入口
scripts/     辅助脚本（setup_external.sh）
tests/       单元测试 + 集成测试（15 文件）
  └─ fixtures/golden/       黄金字节流（真服录制）
external/    第三方依赖（不入 git）
  ├─ helio/                 io_uring + fiber 框架
  ├─ duckdb/                DuckDB vendored（libduckdb.so + duckdb.hpp）
  ├─ googletest/            预下载 tarball
  ├─ abseil/                预下载 tarball
  ├─ benchmark/             预下载 tarball
  ├─ gperf/                 预下载 tarball
  ├─ xxhash/                预下载 tarball
  ├─ uring/                 预下载 tarball
  ├─ pugixml/               预下载 tarball
  ├─ cares/                 预下载 tarball
  ├─ zstd/                  预下载 tarball
  ├─ rapidjson/             git checkout（header-only，离线构建用）
  ├─ expected/              git checkout（header-only，离线构建用）
  ├─ zlib-ng/               zlib-ng 2.3.3 tarball（替代系统 zlib）
output/      程序输出（不入 git）
```
### 2026-06-23 v0.6.0

**zlib-ng 替代系统 zlib**：`find_package(ZLIB)` → FetchContent zlib-ng 2.3.3（ZLIB_COMPAT 模式），API 100% 兼容，零代码改动。消除项目最后一个系统包依赖，实现完全 vendored 离线可复现构建。

**external/ 离线完善**：
- rapidjson / expected-lite：bare repo → 完整 git checkout（`external/rapidjson/`、`external/expected/`），cmake configure 期自动复制到 build tree 并创建 stamp 跳过 ExternalProject 下载。
- 新增 `external/zlib-ng/`（zlib-ng 2.3.3 tarball）。

**代码简化（ponytail audit）**：
- 合并 4 处 `push_u8/u16/u32/push_code` 重复定义到 `byte_order.hpp`（-30 行）。
- 合并 3 处 `DefaultHosts()` 重复定义到 `StdQuotes::DefaultHosts()`（公开，-60 行）。
- Calendar `IsHoliday` O(n) vector → O(1) unordered_set。

**文件变更**（15 files, +193/-139）：
```
CMakeLists.txt                         zlib-ng FetchContent + 离线复制逻辑
include/tdx/util/byte_order.hpp        +push_* 共享 helper
include/tdx/quotes/std_quotes.hpp      DefaultHosts 公开
include/tdx/data/calendar.hpp          vector → unordered_set
src/CMakeLists.txt                     ZLIB::ZLIB → zlib
src/proto/{parsers,ex_parsers,sp_parsers,parsers_quotes}.cpp  去重
src/cli/main.cpp                       复用 DefaultHosts
src/batch/batch_fetch.cpp              复用 DefaultHosts
src/data/calendar.cpp                  O(1) lookup + snprintf 修复
scripts/setup_external.sh              +zlib-ng, rapidjson/expected 完整 clone
CLAUDE.md/README.md                    文档更新
```
