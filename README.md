# tdx-cpp

通达信行情数据 C++17 单库——TCP 协议帧编解码、A股/期货/港美股行情解析（K线/分时/逐笔/五档）、本地 vipdoc 读取、复权因子自算、TDengine 存储导入、断点续传。

## 快速开始

```bash
bash scripts/setup_external.sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
ctest --test-dir build -j$(nproc) --output-on-failure
```

## CLI 命令

> **所有 code 必须带市场前缀 `sh`/`sz`/`bj`**（如 `sh000001`=上证指数、`sz000001`=平安银行），消除歧义 code 的市场错判（v0.13.8 起强制，移除 MarketFromCode 前缀推断）。

| 命令                                                                                                                                                                                                                            | 说明                                                                                                                                  |
| ----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- | ----------------------------------------------------------------------------------------------------------------------------------- |
| `tdx import --tdx-root <path>`                                                                                                                                                                                                | 本地 vipdoc 导入 TDengine（码表驱动 + 网络回退）                                                                                                  |
| `tdx fetch-kline <code> [code...] [periods] [count]`                                                                                                                                                                          | 当日 K 线循环刷新入库（1d/5m/1m，默认循环）                                                                                                       |
| `tdx fetch-quotes [--loop] ...]`                                        | 实时行情采集入库。**默认采集自选股 `zxg.blk`
                                                                          |
| `tdx fetch-names`                                                                                                                                                                                                             | 同步股票代码→名称对照表                                                                                                                        |
| `tdx check-names`                                                                                                                                                                                                             | 检查名称表覆盖完整性                                                                                                                          |
| `tdx cleanup`                                                                                                                                                                                                                 | 清理对照表中已失效的冗余条目                                                                                                                      |
| `tdx truncate-quotes`                                                                                                                                                                                                         | 清空实时行情表                                                                                                                             |
| `tdx fetch-finance <code>`                                                                                                                                                                                                    | 财务数据入库（流通股本/总股本/每股收益等）                                                                                                              |
| `tdx fetch-f10 <code>`                                                                                                                                                                                                        | F10 基本资料入库                                                                                                                          |
| `tdx history-orders <code> <YYYYMMDD>`                                                                                                                                                                                        | 历史委托队列                                                                                                                              |
| `tdx history-tx <code> <YYYYMMDD>`                                                                                                                                                                                            | 历史逐笔成交                                                                                                                              |
| `tdx vol-profile <code>`                                                                                                                                                                                                      | 盘中成交量分布                                                                                                                             |
| `tdx index-info <code>`                                                                                                                                                                                                       | 指数信息（涨跌家数/成分股订单）                                                                                                                    |
| `tdx unusual [market=1]`                                                                                                                                                                                                      | 主力异动监控                                                                                                                              |
| `tdx board-list [type=1]`                                                                                                                                                                                                     | SP/MAC 板块列表                                                                                                                         |
| `tdx board-quotes <board_code>`                                                                                                                                                                                               | 板块成员报价                                                                                                                              |
| `tdx capital-flow <code>`                                                                                                                                                                                                     | 资金流向（主力/散户净额）                                                                                                                       |
| `tdx server-test`                                                                                                                                                                                                             | 服务器测速选服                                                                                                                             |

## 导入覆盖

| 类型         | 代码段                            | 状态   |
| ---------- | ------------------------------ | ---- |
| A 股（沪深京）   | `0/3/6/4/8xxxxx`               | ✅    |
| 板块/沪深指数    | `88xxxx` / `399xxx`            | ✅    |
| ETF/LOF 基金 | `5xxxxx` / `159xxx` / `16xxxx` | ✅    |
| 债券         | `1xxxxx`(非159/16)、`2xxxxx`     | ❌ 排除 |
| B 股        | `2/9xxxxx`                     | ❌ 排除 |
| 港股通        | `7xxxxx`                       | ❌ 排除 |

## 技术栈

C++17 / CMake + Ninja / helio (io\_uring+fiber) / TDengine / Boost.Context / OpenSSL / zlib / iconv / absl::Time

## 版本

当前 `0.15.6`。版本号位于 `CMakeLists.txt` 的 `project(tdx-cpp VERSION x.y.z)`。

### 2026-07-10 v0.15.6

*   **修复 `scripts/fetch-today.py`**：codes-file 支持空格 / 换行两种分隔（修复"整行当 1 个 code"的 bug）；子进程 stderr 改用 `mkstemp` 单一 fd（消双重 fd 泄漏）+ Linux 下 `stdbuf -eL` 强制行缓冲防 abort 丢 trace；异常退出自动 dump stderr 环形缓冲定位根因；Ctrl-C 等待 8s grace 再补 terminate，避免与 tdx 子进程清理竞争。

### 2026-07-10 v0.15.5

*   **新增编排脚本 `scripts/fetch-today.py`**：并行启动 `fetch-kline`（当日 K 线循环入库）+ `fetch-quotes --quote_loop`（实时行情），后台线程每 60s 汇总进度报告；codes 来自 zxg.blk / `--codes` / `--codes-file`，可选 `--mmap`；任一子进程退出或 Ctrl-C 优雅终止。

### 2026-07-10 v0.15.4

*   **移除已回退命令的 CLI 分发**：`batch-fetch` / `bars` / `ex-bars` / `fetch-history` / `pull-kline` 全部落入「未知命令」（`batch-fetch` 回落为测试用例，`BatchFetchKline` 保留在 `src/batch/`；其余功能已被其他命令替代）；`tdx` 链接移除 `tdx_batch`。
*   **文档清理**：`README.md` 移除 `batch-fetch`（已回退）与 `quotes_reader`（已被 `tools/zxg_viewer` 替代）行、格式整理；`CLAUDE.md` 清理 `batch-fetch`/`quotes_reader` CLI 引用、`tdx_batch` 标注为「已从 CLI 移除」。
