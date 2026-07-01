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

| 命令 | 说明 |
|---|---|
| `tdx bars <code> [period] [count]` | A 股 K 线（支持前/后复权） |
| `tdx ex-bars <market> <code> [period] [count]` | 扩展行情 K 线（期货/港美股） |
| `tdx batch-fetch --stock-list <file> --start <date> --end <date> -n <N>` | 并发批量拉取 + 断点续传 |
| `tdx import --tdx-root <path>` | 本地 vipdoc 导入 TDengine（码表驱动 + 网络回退） |
| `tdx pull-kline <code> [code...]` | 网络拉取 K 线(1d/1m/5m) 补导缺失代码 |
| `tdx sync-names` | 同步股票代码→名称对照表 |
| `tdx check-names` | 检查名称表覆盖完整性 |
| `tdx cleanup` | 清理对照表中已失效的冗余条目 |
| `tdx fetch-quotes [--loop] [--codes ...] [--with_tx] [--with_tick] [--with_index] [--with_unusual] [--with_finance] [--with_f10] [--with_vol] [--with_hist]` | 实时行情采集入库（多类型，支持循环） |
| `tdx truncate-quotes` | 清空实时行情表 |
| `tdx finance <code>` | 财务数据（流通股本/总股本/每股收益等） |
| `tdx f10 <code>` | F10 基本资料分类目录 |
| `tdx history-orders <code> <YYYYMMDD>` | 历史委托队列 |
| `tdx history-tx <code> <YYYYMMDD>` | 历史逐笔成交 |
| `tdx vol-profile <code>` | 盘中成交量分布 |
| `tdx index-info <code>` | 指数信息（涨跌家数/成分股订单） |
| `tdx unusual [market=1]` | 主力异动监控 |
| `tdx board-list [type=1]` | SP/MAC 板块列表 |
| `tdx board-quotes <board_code>` | 板块成员报价 |
| `tdx capital-flow <code>` | 资金流向（主力/散户净额） |
| `tdx server-test` | 服务器测速选服 |

## 导入覆盖

| 类型 | 代码段 | 状态 |
|---|---|---|
| A 股（沪深京） | `0/3/6/4/8xxxxx` | ✅ |
| 板块/沪深指数 | `88xxxx` / `399xxx` | ✅ |
| ETF/LOF 基金 | `5xxxxx` / `159xxx` / `16xxxx` | ✅ |
| 债券 | `1xxxxx`(非159/16)、`2xxxxx` | ❌ 排除 |
| B 股 | `2/9xxxxx` | ❌ 排除 |
| 港股通 | `7xxxxx` | ❌ 排除 |

## 技术栈

C++17 / CMake + Ninja / helio (io_uring+fiber) / TDengine / Boost.Context / OpenSSL / zlib / iconv / absl::Time

## 版本

当前 `0.11.0`。版本号位于 `CMakeLists.txt` 的 `project(tdx-cpp VERSION x.y.z)`。
