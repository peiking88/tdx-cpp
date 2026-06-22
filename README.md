# tdx-cpp

通达信（TDX）行情数据 C++ 单库。用 C++ 原生重写 [opentdx](https://github.com/rainx/opentdx)（协议层）+ [mootdx](https://github.com/mootdx/mootdx)（行情接口层）+ [tdxdata](https://github.com/rainx/tdxdata)（数据管理层），消除 Python 运行时依赖，获得确定性性能与协议字节级正确性。

异步 IO 采用 [helio](https://github.com/romange/helio)（io_uring + fiber 协程，C++17）。

## 当前状态：Phase 1 完成（v0.1.0）

| 能力 | 状态 |
|---|---|
| 协议层（帧编解码 / 变长 codec / Connection / Retry+CircuitBreaker / Heartbeat / ServerPool） | ✅ |
| A 股核心 Parser（K线 / 分时 / 逐笔 / 五档 / 列表 / 数量 / 登录 / 心跳） | ✅ |
| 本地 vipdoc（.day / .lc1 / .lc5） | ✅ |
| StdQuotes 集成 API + helio io_uring + 自动选服 + 熔断保护 | ✅ |
| CLI（server-test / bars） | ✅ |
| 黄金字节流测试（4 个 P0 fixtures，真服录制） | ✅ |
| live 连真服拉 600000 日 K | ✅ |

## 构建

依赖：helio（内嵌 `add_subdirectory`）、系统 Boost（context）、zlib、iconv、GoogleTest（helio 提供）。

```bash
# 配置（Release，ninja）
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
# 编译（指定 target，避免 build all 触发 helio 全部第三方）
cmake --build build --target tdx -j$(nproc)
```

helio 源码路径可用 `-DHELIO_PATH=<path>` 或环境变量 `HELIO_PATH` 配置（默认 `/home/li/framework/dragonfly/helio`）。

## 用法

```bash
# 测速标准行情服务器
./build/bin/tdx server-test

# 拉取 K 线（代码 周期 数量；周期 0=5分 4=日 5=周 6=月 7=1分 9=多日 11=年）
./build/bin/tdx bars 600000 4 10
```

## 测试

```bash
cmake --build build --target test_codec test_frame test_util test_parsers test_vipdoc_reader test_golden test_retry test_server_pool -j$(nproc)
ctest --test-dir build -R "^test_" -j$(nproc) --output-on-failure
```

黄金字节流 fixtures 由 `scripts/record_golden.py` 录制（连真服抓响应 body + opentdx 解析生成 expected）：

```bash
python scripts/record_golden.py kline 600000
```

## 架构

三层分层（依赖自下而上）：

```
CLI (tdx) → tdx::quotes::StdQuotes → tdx::proto（协议层）→ tdx::util + helio
```

- `tdx::util`：iconv GBK、zlib、字节序、时间工具
- `tdx::proto`：帧编解码（`<BIBHH`/`<IBIBHHH`）、变长 codec（get_price/to_datetime）、Connection（helio FiberSocket）、RetryPolicy+CircuitBreaker、Heartbeat、ServerPool、Parser（逐字移植 opentdx）、vipdoc 读取
- `tdx::quotes::StdQuotes`：组合协议层，提供 bars/quotes/transactions/stocks 语义化 API

**helio fiber 纪律**：协议层全部在 Proactor 线程/fiber 内执行，禁用 `std::mutex`/`std::this_thread::sleep_for`，统一用 `util::fb2::Mutex`/`ThisFiber::SleepFor`（详见 `CLAUDE.md`）。

## 目录结构

```
docs/      API 文档、协议说明、PRD
cfg/       服务器列表（servers.json，从 opentdx const.py 提取）
include/   公共头（tdx/types.hpp, consts.hpp, proto/*, util/*）
src/       协议层 / 行情接口层 / CLI
scripts/   测速、黄金字节流录制
tests/     单元测试 + 黄金字节流 fixtures
output/    程序输出（不入 git）
```

## 后续阶段

- Phase 2：扩展行情（期货/港美股/期权）+ SP/MAC 高级行情（板块/资金流）
- Phase 3：数据管理层（TdxData 统一 API、熔断+增量同步、复权、时段重采样）
- Phase 4 (v2)：并发批量、断点续传、Dragonfly 热缓存 + Parquet 冷存储分层

详见 `.claude/PRPs/prds/tdx-cpp.prd.md`。
