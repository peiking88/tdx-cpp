# tdx-cpp

通达信行情数据 C++17 单库——TCP 协议帧编解码、A股/期货/港美股行情解析（K线/分时/逐笔/五档）、本地 vipdoc 读取、复权因子自算、TDengine 存储导入、断点续传。

## 快速开始

```bash
bash scripts/setup_external.sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
ctest --test-dir build -j$(nproc) --output-on-failure
```

## 辅助脚本

```bash
python3 scripts/smmd.py --zxg   # SMMD 股市操纵检测（K-Means++ 聚类 + 启发式欺诈分类）
python3 scripts/append.py       # 全市场增量补导（读 stock_name → vipdoc + 网络增量入库）
python3 scripts/scalper.py      # 隔夜套利选股（14:30-15:00 循环, 写 WP.blk + scalper_pick + scalper-日期.csv）
python3 scripts/scalper.py verify   # 选股验证（前日14:30-15:00均价买/当日9:30-10:00均价卖, 写 scalper_verify + verify.csv）
python3 scripts/find-diverse.py --zxg     # 多策略选股筛子（--methods leader macd-week-dc macd-min-gc 三法 OR 合并, 写 LT.blk + find-diverse.csv）
python3 scripts/find-diverse.py --zxg --methods macd-min-gc             # 仅分钟信号（30m MACD底背离 + 5m MACD零轴金叉）
python3 scripts/find-diverse.py --zxg --backtest-div            # 回测分钟信号各持有期收益率（vs 全 bar 基线）
python3 scripts/find-reversal.py                          # 底部反转筛选（默认自选股, --all 全 A 股; 600105 模式）
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