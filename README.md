# tdx-cpp

通达信行情数据 C++17 单库——TCP 协议帧编解码、A股/期货/港美股行情解析（K线/分时/逐笔/五档）、本地 vipdoc 读取、复权因子自算、TDengine 存储导入、断点续传。

## 快速开始

```bash
bash scripts/setup_external.sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
ctest --test-dir build -j$(nproc) --output-on-failure
```

## 技术栈与依赖

| 依赖 | 用途 | 备注 |
|---|---|---|
| C++17 + CMake + Ninja | 构建 | `setup_external.sh` 初始化 |
| TDengine 客户端 | 时序存储 | libtaos（系统包 `libtaos-dev`） |
| ta-lib | 技术指标（MACD/RSI/BOLL 等） | **czsc 模块必需**（`sudo apt install ta-lib-dev libta-lib0`） |
| helio | 异步 IO（io_uring + fibers） | 行情网络层 |
| nlohmann/json | JSON 序列化 | vendored（`include/nlohmann`，含 json_fwd.hpp） |
| zlib / iconv | 压缩 / GBK 转码 | 协议必需 |

**czsc 缠论模块**（`include/czsc` + `src/czsc`，并入自 [czsc-cpp](https://github.com/waditu/czsc) v0.7.0）提供分型/笔/中枢识别与 246 个信号函数；纯计算库，**不链 helio/absl**，taos 连接走 `tdx_taos_conn` 叶子。

## 辅助脚本

```bash
python3 scripts/smmd.py --zxg   # SMMD 股市操纵检测（K-Means++ 聚类 + 启发式欺诈分类）
python3 scripts/append.py       # 全市场增量补导（读 stock_name → vipdoc + 网络增量入库）
python3 scripts/append.py --czsc  # 盘后批处理: import + czsc 缠论信号落库（signals 表）
python3 scripts/scalper.py      # 隔夜套利选股（14:30-15:00 循环, 写 WP.blk + scalper_pick + scalper-日期.csv）
python3 scripts/scalper.py verify   # 选股验证（前日14:30-15:00均价买/当日9:30-10:00均价卖, 写 scalper_verify + verify.csv）
python3 scripts/find-diverse.py --zxg     # 多策略选股筛子（--methods leader macd-week-dc macd-min-gc 三法 OR 合并, 写 LT.blk + find-diverse.csv）
python3 scripts/find-diverse.py --zxg --methods macd-min-gc             # 仅分钟信号（30m MACD底背离 + 5m MACD零轴金叉）
python3 scripts/find-diverse.py --zxg --backtest-div            # 回测分钟信号各持有期收益率（vs 全 bar 基线）
python3 scripts/find-reversal.py                          # 底部反转筛选（前复权, 600105 模式; --backtest 滑窗回测; --market-bull 大盘滤网）
python3 scripts/find-retrace.py                            # 昨日涨停今低开筛选（前复权, 现价距一年内最低点 <=10%; --backtest 滑窗回测; --market-bull）
python3 scripts/find-byslope.py --all                   # 均线斜率评估: 金叉按MA5斜率分桶回测前瞻收益; --today 出当日金叉清单; --market-bull
python3 scripts/find-terrain.py --all                   # 相控阵地貌选股（GMMA等高线+波束扫描九类地貌; 反转筛选漏斗: 下降谷底收敛领衔; --backtest 事件回测 / --calibrate 阈值走步标定; --market-bull）
python3 scripts/find-trivol.py --all              # 寻一三倍量战法选股（V≥前日×3收阳+近5日均量3~10倍; 红线=三倍量收盘价, EXPMA12上穿红线金叉/缩量回调低吸/突破三分类; --backtest 规则出场回测; --market-bull; 建议尾盘运行）
python3 scripts/find-finish-eating.py --all                   # 吸筹结束突破筛选（平台+低位+地量+筹码集中+情绪+股东户数+放量破颈线; --backtest 滑窗回测; --market-bull; 建议尾盘运行）
python3 scripts/find-bottom.py --all              # 抄底信号扫描（DEMA20跌破买入区/RSI14上穿30卖出; --backtest 回测 vs 买入持有; --market-bull）
python3 scripts/find-combo.py --min-hit 2             # 综合选股: 5策略共识（terrain+trivol+weekdc+reversal+finish-eating; --min-hit N 共振筛选; --market-bull）
python3 scripts/find-wave.py --code sh600276 --cycle 1d   # 波浪分析：波峰波谷识别 + 涨跌力度（量价/幅度/时间）
python3 scripts/fetch-margin.py                            # 融资融券数据入库（AKShare → TDengine）
python3 scripts/leverage-risk.py                           # 市场杠杆风险监测（读 TDengine，零外部依赖）
python3 scripts/find-jerry.py                               # 某队 ETF 动向追踪（汇金/证金/社保持仓变化）
python3 scripts/market-analysis.py                           # 盘面分析（财经资讯 + LLM 生成报告; 需 DEEPSEEK_API_KEY; → output/market-analysis/）
python3 scripts/czsc-predict.py                              # 缠论趋势预测（1d/30m/5m, 复用C++ czsc引擎; → output/czsc/）
```

## 选股与验证持久化

选股与验证结果同步落本地（csv/blk）与 TDengine（带日期戳, 方便查询统计）：

| TDengine 超级表 | 子表 | 内容 |
|---|---|---|
| `scalper_pick` | `sp_{market}{code}` | 每日选股命中（涨幅/量比/换手/市值/VWAP/信号得分 + enhancement_json 全量信号） |
| `scalper_verify` | `sv_{market}{code}` + `sv_total` | 验证收益（买卖均价/股数/成本/盈亏/收益率, 含合计行） |

```sql
SELECT DATE(ts) d, AVG(pnl_pct) FROM scalper_verify WHERE code<>'TOTAL' GROUP BY d ORDER BY d;  -- 战法每日表现
SELECT code, signal_score FROM scalper_pick WHERE ts='2026-07-30';                              -- 某日选股
```