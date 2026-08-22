# scalper.py 深入:尾盘动量因子数学 与 verify 买卖窗口逻辑

> 配套 `scripts/scalper.py`(隔夜套利/时空置换战法选股)。本文展开两块易被略读、但决定选股质量与回测可信度的逻辑:① 增强信号里的**尾盘动量因子**如何从分钟线算出一个 `cos` 系数;② `verify` 子命令的**买卖时间窗 + 收益公式**如何对应战法语义。所有行号对应仓库根 `scripts/scalper.py`。

---

## 一、尾盘动量因子 `calc_tail_momentum_factor`

位置:`scripts/scalper.py:428-477`。输入 `bars_intraday_ndays`(N 天 1 分钟 close,SQL 端已预过滤 13:31 后,见 `batch_query_intraday_ndays:285-289`),输出 `{cos, ar, days}` 或 `None`。

### 1.1 目的

隔夜套利只关心一件事:**尾盘拉升能否延续到次日早盘**。该因子用历史多日 13:31 之后的分钟收益率序列,度量「正收益之后下一刻是否继续为正」的统计惯性,作为 `cos > 0` 的加分项(`diagnose:760-762`)。

### 1.2 数据流

```
N 日 1m (仅 13:31 后 close)
   │
   ├── 按交易日分组  daily[YYYYMMDD] = [close...]
   ├── 取最近 lookback(默认 20)天
   │
   └── 逐日计算 ──► cos_list[], ar_list[] ──► 各取均值 → {cos, ar, days}
```

### 1.3 数学推导(逐日)

**① 分钟收益率**(要求当日 ≥10 根 bar,`scalper.py:444`):

$$
r_i = \frac{c_i}{c_{i-1}} - 1,\quad i = 1\ldots n-1
$$

**② 去均值**(中心化,消除当日整体涨跌的基线,`scalper.py:450-451`):

$$
e_i = r_i - \bar r,\qquad \bar r = \frac{1}{n}\sum_i r_i
$$

**③ 非对称采样**(本因子最关键、最 ad-hoc 的一步,`scalper.py:452-457`):

$$
(x_k,\, y_k) = (e_i,\, e_{i+1})\ \text{仅当}\ e_i > 0
$$

即只保留「当前去均值为正」的时刻,把它与下一刻的去均值收益配对。负收益的样本对**直接丢弃**。这不是教科书自相关,而是「**正收益延续概率**」的代理——恰好匹配战法只关心尾盘拉升延续、不关心下跌延续的语义。

**④ 余弦相似度 `cos`**(`scalper.py:458-463`,注意用原始 x、y,**不再减均值**):

$$
\cos = \frac{\sum_k x_k y_k}{\sqrt{\sum_k x_k^2}\;\sqrt{\sum_k y_k^2}}
$$

- 因 $x_k > 0$(过滤保证),$\cos$ 的符号由 $y_k$ 主导。
- $y_k = e_{i+1} > 0$(正收益后继续正)居多 → $\cos > 0$ → 上涨有惯性。
- $y_k < 0$(正收益后反转)居多 → $\cos < 0$ → 上涨后回调。
- $\cos \in [-1, 1]$。

**⑤ AR(1) 系数 `ar`**(`scalper.py:464-469`,**这里才减均值**,标准 OLS 过原点回归斜率):

$$
\ar = \beta = \frac{\sum_k (x_k-\bar x)(y_k-\bar y)}{\sum_k (x_k-\bar x)^2}
$$

- 衡量 $y$ 对 $x$ 的线性依赖强度,无界(可 >1 或 <−1)。
- 与 $\cos$ 的差别:$\cos$ 归一化到 $[-1,1]$ 看「方向一致性」;$\ar$ 是回归系数看「弹性/幅度」。两者同号时互相印证。

**⑥ 跨日聚合**(`scalper.py:473-477`):对最近 `lookback` 天每天各算一个 $\cos$、$\ar$,取算术均值;`days` = 有效样本天数。

### 1.4 信号判定

```python
# scalper.py:760-762
("tail_mom", lambda: ... enhancements["tail_mom"]["cos"] > 0),
```

**只用 `cos > 0`** 作为命中(加 1 分),`ar` 仅供 `render_table` 展示(`scalper.py:901-905`)。门槛很宽松:只要历史尾盘正收益「略微偏向延续」即命中。

### 1.5 设计意图与局限(如实标注)

- **领域适配合理**:非对称采样对齐「只在乎拉升延续」的战法语义,比对称自相关更贴题。
- **样本量折半**:只取 $e_i>0$ 的对,样本数约为全样本一半,分钟噪声下统计显著性偏弱。
- **算术跨日均值**:对个别异常日敏感,未做加权或中位数稳健化。
- **门槛过松**:`cos > 0` 区分度低,大量标的会命中,实际区分力依赖与其他增强信号的叠加 `signal_score`。
- `ponytail:` 注释:这是战法导向的经验指标,非严谨时序统计量;升级路径——改用全样本自相关 + 显著性检验,或对 $\cos$ 设更高阈值(如 0.3)再加分。

---

## 二、`verify` 买卖窗口逻辑

位置:`run_verify`(`scripts/scalper.py:975-1045`),核心循环 `1003-1030`,窗口均价 `window_avg:355-361`。

### 2.1 战法对应

| 动作     | 窗口                       | 语义                                                         |
| -------- | -------------------------- | ------------------------------------------------------------ |
| **买入** | 上一交易日 **14:30–15:00** | 尾盘选股信号触发的「尾盘买入」,正是 scalper 选股模式运行时段 |
| **卖出** | 验证当日 **09:30–10:00**   | 「次日早盘清仓」铁律(持股不超 4 小时,见 `render_table:937`)  |

两价均为窗口内**分钟 close 的算术平均**(非 VWAP),规避单根 bar 噪声。

### 2.2 买入侧

```python
# scalper.py:1003-1007
bars = fetch_recent_1m(conn, market, code, args.days)        # 最近 days 天 [(ts, close)]
dates_before = sorted({ts.date() for ts,_ in bars if ts.date() < today})
buy_date = dates_before[-1] if dates_before else None        # 数据驱动的「上一交易日」
buy, bn = window_avg(bars, buy_date, 1430, 1500) if buy_date else (None, 0)
```

- `buy_date` **从实际有数据的日期推断**,而非日历日——自动跳过停牌/节假日,鲁棒。
- 无 `buy_date`(数据不足)→ 该标的计为「缺数据」,不计入盈亏。

### 2.3 卖出侧

```python
# scalper.py:1008
sell, sn = window_avg(bars, today, 930, 1000)                # 当日 09:30–10:00 均价
```

`window_avg`(`scalper.py:355-361`)的实现:筛选 `ts.date() == target_date` 且 `hm_lo ≤ HHMM < hm_hi` 的 close,取算术均值,同时返回命中 bar 数 `n`(写入 `buy_bars/sell_bars` 便于诊断数据完整性)。

### 2.4 收益计算

资金 `VERIFY_CAPITAL = 100000` 元等额分配到每只标的(`scalper.py:1000-1001`):

$$
\text{per} = \frac{\text{capital}}{N},\quad
\text{shares} = \frac{\text{per}}{P_{\text{buy}}},\quad
\text{income} = \text{shares}\times P_{\text{sell}}
$$

$$
\text{pnl} = \text{income} - \text{per},\qquad
\text{pnl\_pct} = \frac{P_{\text{sell}} - P_{\text{buy}}}{P_{\text{buy}}}\times 100\%
$$

**内在关系**(可由上式消去 shares 推得):

$$
\text{pnl} = \text{per} \times \frac{\text{pnl\_pct}}{100}
$$

即每只 pnl 是其涨跌幅 × 分配资金;TOTAL 行 `pnl_pct = Σpnl / capital × 100`(`scalper.py:1032, 1039`),与个股权重一致,可直接读作组合收益率。

### 2.5 等待逻辑(`scalper.py:997-998`)

```python
if not args.no_wait and not args.date:
    wait_until(today_at(10, 0))          # 实时验证:等到 10:00 再算
```

- **实时跑**(无 `--date`)且未加 `--no-wait`:阻塞到 10:00,确保 09:30–10:00 卖出窗数据完整,避免半窗低估均价。
- **回测**(`--date` 指定历史日)或 `--no-wait`:立即用已有数据计算(回测远日时需调大 `--days` 拉足分钟线)。

### 2.6 简化与局限(代码已标注)

`scalper.py:1022` 的 `ponytail:` 注释明确列出三个未建模项,回测为**理想化上界**:

1. **连续股数不取整**:`shares = per / buy` 不取整到 100 股最小单位 → 小资金/高价股偏差大。
2. **不计手续费**:券商佣金(通常 ≤万分之2.5,双边)。
3. **不计印花税**:卖出单边 1‰(2023 年起减半至 0.5‰,以当时政策为准)。

实盘净值会系统性低于该回测 `pnl`。升级路径见注释——引入 100 股取整 + 双边费率模型即可贴近真实。

---

## 附:代码索引

| 主题                   | 位置                          |
| ---------------------- | ----------------------------- |
| 动量因子主体           | `scripts/scalper.py:428-477`  |
| 13:31 预过滤 (SQL 端)  | `scripts/scalper.py:285-289`  |
| cos 命中判定           | `scripts/scalper.py:760-762`  |
| cos/ar 表格渲染        | `scripts/scalper.py:901-905`  |
| verify 主循环          | `scripts/scalper.py:975-1045` |
| 窗口均价 window_avg    | `scripts/scalper.py:355-361`  |
| 连续股数 ponytail 注释 | `scripts/scalper.py:1022`     |
| 等待 10:00             | `scripts/scalper.py:997-998`  |
| 持股铁律提示           | `scripts/scalper.py:937`      |
