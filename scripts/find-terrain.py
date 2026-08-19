#!/usr/bin/env python3
"""
================================================================================
 相控阵地形测绘选股 — GMMA 等高线 + 波束扫描地貌分类（诊断 + 回测 + 标定）
================================================================================

源自四方茶院《趋势不只有涨跌：用相控阵"测绘"金融市场》(2026-08-14)。文章只给
了方法框架，地貌分类阈值未公开——本文阈值自设（文件头常量区），标定见 --calibrate。

算法链（对齐文章）:
  1. GMMA 六组快慢 EMA 对: (15,30)(12,35)(10,40)(8,45)(5,50)(3,60)
     gap_n = EMA_fast - EMA_slow，ATR14 归一化 → 六层「等高线」(ATR 单位)
  2. 复相量 z_n = gap_n + j·v_n，v_n = (gap_n - gap_n[t-5])/5×10
     （过去 5 日变化率投影到 10 日尺度 = 地形隆起/塌陷速度）
  3. 波束扫描 B(β) = mean(z_n·exp(-jβx_n))，β∈[-90°,+90°] 1° 步进，
     x_n = 阵元位置（EMA 对有效滞后 (f+s-2)/4 归一到 [0,1]）
     β* = argmax|B| 为主波束方向；相干度 = |B(β*)|/mean|z_n| ∈ [0,1]
  4. 地貌分类: 高程/时向坡度/相干度/新鲜度 九类决策树（见 classify_arr）

三种模式:
  默认        当日地貌屏显 + xlsx（诊断/候选清单）
  --backtest  事件回测: 各地貌「首次进入日」→ +5/+20/+65 日前瞻收益分布
              （事件取 class-run 首日, 天然去重叠; 前复权含分红）
  --calibrate 阈值走步标定: (ELEV_ZERO×SLOPE_FRAC) 网格, 内样本 70% 选参,
              外样本 30% 验证（ponytail: 单分裂走步, 滚动多窗要再说）

策略定位: 反转筛选漏斗（全市场事件回测 2024-05~2026-08, 35.6 万事件实证）
  漏斗工序  「下降谷底收敛」（可叠「向上转折带」确认）→ 候选池, +65 日持有
            口径; 超额 +3.1pp / 胜率 63.9% vs 全体 57.3%
  仅作诊断  动量类地貌（上升陡坡形成 +65 日不跑赢全体, 追涨不成立）; 顶钝化
            MAE -9.7% 最深 → 持仓减仓信号
  风险提示  OOS 胜率 46.4%（IS 58.5%）: 收益右偏, 少数大赢家撑均值, 多数
            事件小亏——需止盈/分散, 不能单吊重仓

已知局限（文章自述 + A 股适配）:
  六阵元同源于一条价格 → 高相干度≠独立共识（实测全市场相干度≈1 饱和）;
  连板/一字板压缩 ATR 使地貌失真; 上市<120 交易日跳过（EMA60 未稳 + 次新）。

用法:
  python3 scripts/find-terrain.py                       # 自选股当日地貌
  python3 scripts/find-terrain.py --all                 # 全 A 股
  python3 scripts/find-terrain.py --all --backtest      # 全市场事件回测
  python3 scripts/find-terrain.py --all --calibrate     # 阈值走步标定
  python3 scripts/find-terrain.py --self-test           # 合成数据自检
================================================================================
"""

import argparse
import os
import sys
import time
import warnings
from datetime import datetime, timedelta

import numpy as np
import pandas as pd
import taosws

from common import (OUTPUT_DIR, all_mainboard_codes, apply_qfq, batch_fetch_adjust,
                    parse_code, zxg_codes)

# ======================== 配置 ========================
TDENGINE_URL = os.environ.get("TDENGINE_URL", "taosws://root:taosdata@localhost:6041/tdx")

BATCH_SIZE = 1800
MIN_ROWS = 120          # 上市/数据不足直接跳过（EMA60 稳定 + 次新过滤）
LOOKBACK_DAYS = 160     # 当日模式加载交易日数
BACKTEST_DAYS = 500     # 回测/标定默认回看交易日（覆盖 +65 日前瞻）
FWD_PERIODS = (5, 20, 65)   # 前瞻收益区间（≈1/4/13 周, 交易日）
CONFIRM_DAYS = 3       # 事件确认: 地貌 run 不足 3 日不计事件（滤 1 日翻覆抖动,
                        # 自检实测纯上升坡在延续/顶钝化边界高频抖动）

# --- GMMA 六组快慢对（文章原参数） ---
GMMA_PAIRS = ((15, 30), (12, 35), (10, 40), (8, 45), (5, 50), (3, 60))
ATR_N = 14              # 归一化分母
RATE_WIN = 5            # gap 变化率回看窗（日）
RATE_PROJ = 10          # 投影尺度（日）

# --- 阵元位置: EMA(span N) 群延迟≈(N-1)/2，对取均值，归一到 [0,1] ---
X_POS = np.array([(f + s - 2) / 4.0 for f, s in GMMA_PAIRS])
X_POS = (X_POS - X_POS.min()) / (X_POS.max() - X_POS.min())
BETAS = np.deg2rad(np.arange(-90, 91, 1))   # 扫描相位梯度（度→弧度）
_W_BEAM = np.exp(-1j * np.outer(X_POS, BETAS))   # [6, 91] 波束扫描权矩阵

# --- 地貌分类阈值（默认值; 标定见 --calibrate） ---
BROKEN_COH = 0.55       # 相干度低于此 → 断裂杂波
ZERO_COH = 0.80         # 零区相干度高于此才认「转折带」
ELEV_ZERO = 0.15        # |高程| 低于此视为分水岭零区（平坦噪声高程底 ~±0.12 ATR,
                        #  自检实测; 定 0.10 会把横盘误判弱趋势）
SLOPE_ACT = 0.02        # 时向坡度激活基础阈值（10 日投影口径）
SLOPE_FRAC = 0.05       # 激活线随高程放大: act = SLOPE_ACT + 5%·|高程|/日
                        # （gap 噪声与高程同量级，纯绝对阈值会被噪声击穿——自检教训）

# --- 标定网格与走步分裂 ---
CAL_EZ = (0.10, 0.15, 0.20)
CAL_SF = (0.03, 0.05, 0.10)
IS_FRAC = 0.70          # 内样本占比（按事件日期分位切）
CAL_MIN_N = 200         # 内样本每组合最小事件数

# 分类代码 → 名称（下标即 classify_arr 输出代码, 顺序不可改）
CLASS_ORDER = ["上升陡坡形成", "上升坡延续", "向上转折带", "上升坡顶钝化",
               "向下转折带", "下降谷底收敛", "下降坡延续", "下降陡坡形成",
               "地形转换区", "断裂杂波地形"]

# 屏显/xlsx 排序优先级: 反转漏斗类在前（策略定位）, 动量类在后（仅诊断）
DISPLAY_ORDER = ["下降谷底收敛", "向上转折带", "向下转折带",
                 "上升陡坡形成", "上升坡延续", "上升坡顶钝化",
                 "下降坡延续", "下降陡坡形成", "地形转换区", "断裂杂波地形"]


# ======================== 数据获取（对齐 find-byslope 惯例） ========================
def get_a_stock_codes(cursor):
    cursor.execute(r'SELECT table_name FROM information_schema.ins_tables WHERE table_name LIKE "k\_%\_1d"')
    out = []
    for t in cursor.fetchall():
        inner = t[0][2:-3]
        market, num = inner[:2], inner[2:]
        if len(num) == 6 and num.isdigit() and market in ("sh", "sz", "bj"):
            out.append((market, num))
    return out


def batch_query_kline(cursor, codes, start_date):
    queries = [
        f"SELECT ts, open, high, low, close, '{market}{num}' as code "
        f"FROM tdx.k_{market}{num}_1d WHERE ts >= '{start_date}'"
        for market, num in codes
    ]
    cursor.execute(" UNION ALL ".join(queries) + " ORDER BY code, ts")
    return cursor.fetchall()


def load_stock_names(conn):
    try:
        return {f"{m}{c}": n for m, c, n in conn.query(
            "SELECT market, code, name FROM tdx.stock_name")}
    except Exception:
        return {}


def iter_stock_frames(cursor, a_stocks, adj_by_mc, start_date, today_str):
    """批量拉 K 线 → 逐只 yield (code, qfq DataFrame[ts,O,H,L,C]，已剔当日未完成 bar)。"""
    total_batches = (len(a_stocks) + BATCH_SIZE - 1) // BATCH_SIZE
    for bi in range(total_batches):
        t2 = time.time()
        batch = a_stocks[bi * BATCH_SIZE:(bi + 1) * BATCH_SIZE]
        by_code = {}
        for row in batch_query_kline(cursor, batch, start_date):
            by_code.setdefault(row[5], []).append(row)
        n_ok = 0
        for market, num in batch:
            code = f"{market}{num}"
            rows = by_code.get(code, [])
            if len(rows) < MIN_ROWS:
                continue
            df = pd.DataFrame([r[:5] for r in rows], columns=["ts", "O", "H", "L", "C"])
            df["ts"] = pd.to_datetime(df["ts"])
            events = adj_by_mc.get((market, num))
            if events:
                df = apply_qfq(df, events)
            df = df[df["ts"].dt.strftime("%Y-%m-%d") != today_str]
            if len(df) < MIN_ROWS:
                continue
            n_ok += 1
            yield code, df
        print(f"[2/3] 批次 {bi+1}/{total_batches}: {time.time()-t2:.1f}s, 有效 {n_ok}/{len(batch)}",
              file=sys.stderr)


# ======================== 相控阵核心 ========================
def _atr14(high, low, close):
    prev = close.shift(1)
    tr = pd.concat([high - low, (high - prev).abs(), (low - prev).abs()],
                   axis=1).max(axis=1)
    return tr.rolling(ATR_N).mean()


def terrain_features(close, high, low):
    """单只标的 → 「当日」地形特征 dict（当日模式用）。数据不足返回 None。"""
    n = len(close)
    if n < MIN_ROWS:
        return None
    atr = _atr14(high, low, close)
    if not np.isfinite(atr.iloc[-1]) or atr.iloc[-1] <= 0:
        return None
    gaps = []
    for f, s in GMMA_PAIRS:
        ef = close.ewm(span=f, adjust=False).mean()
        es = close.ewm(span=s, adjust=False).mean()
        gaps.append(((ef - es) / atr).iloc[-(RATE_WIN + 1):].to_numpy())
    gaps = np.array(gaps)                       # [6, RATE_WIN+1]
    if not np.all(np.isfinite(gaps)):
        return None
    gap_now = gaps[:, -1]
    v_now = (gaps[:, -1] - gaps[:, 0]) / RATE_WIN * RATE_PROJ
    z = gap_now + 1j * v_now
    beam = np.abs(z @ _W_BEAM / 6.0)
    k = int(np.argmax(beam))
    coh = float(beam[k] / np.mean(np.abs(z)))
    beta = float(np.rad2deg(BETAS[k]))
    return dict(elev=float(np.mean(gap_now)), slope_t=float(np.mean(v_now)),
                elev_5=float(np.mean(gaps[:, 0])), coh=coh, beam=beta,
                phase=float(np.rad2deg(np.angle((z @ _W_BEAM / 6.0)[k]))),
                cross=float(np.polyfit(X_POS, gap_now, 1)[0]),
                relief=float(gap_now.max() - gap_now.min()))


def terrain_series(close, high, low):
    """单只标的 → 逐日特征 dict（回测/标定用，向量化）。与 terrain_features 同口径。

    返回 dict(elev, slope_t, elev_5, coh, beam, cross)，各数组长 n-RATE_WIN，
    对齐 close.index[RATE_WIN:]；前 ATR_N 段含 NaN 由事件提取的 valid 掩码剔除。
    """
    n = len(close)
    if n < MIN_ROWS:
        return None
    atr = _atr14(high, low, close)
    gaps = np.column_stack([
        ((close.ewm(span=f, adjust=False).mean() - close.ewm(span=s, adjust=False).mean())
         / atr).to_numpy()
        for f, s in GMMA_PAIRS
    ])                                           # [n, 6]
    g1, g0 = gaps[RATE_WIN:], gaps[:-RATE_WIN]
    v = (g1 - g0) / RATE_WIN * RATE_PROJ
    z = g1 + 1j * v                              # [n-5, 6]
    B = z @ _W_BEAM / 6.0                        # [n-5, 91]
    mag = np.abs(B)
    k = mag.argmax(axis=1)
    idx = np.arange(len(k))
    xc = X_POS - X_POS.mean()
    with warnings.catch_warnings():
        warnings.simplefilter("ignore", RuntimeWarning)   # 前 ATR_N 行全 NaN 的 nanmean
        return dict(
            elev=np.nanmean(g1, axis=1),
            slope_t=np.nanmean(v, axis=1),
            elev_5=np.nanmean(g0, axis=1),
            coh=mag[idx, k] / np.nanmean(np.abs(z), axis=1),
            beam=np.rad2deg(BETAS)[k],
            cross=((xc[None, :] * (g1 - np.nanmean(g1, axis=1, keepdims=True))).sum(axis=1)
                   / (xc ** 2).sum()),
        )


def classify_arr(elev, slope, elev_5, coh, ez=ELEV_ZERO, sf=SLOPE_FRAC):
    """地貌九类决策树（向量化）。返回 CLASS_ORDER 下标。阈值含义见常量区。"""
    act = SLOPE_ACT + sf * np.abs(elev)
    zero = np.abs(elev) < ez
    up, dn = elev > 0, elev < 0
    return np.select([
        coh < BROKEN_COH,                        # 9 断裂杂波
        zero & (coh >= ZERO_COH) & (slope > act),   # 2 向上转折带
        zero & (coh >= ZERO_COH) & (slope < -act),  # 4 向下转折带
        zero,                                    # 8 地形转换区
        up & (slope > act) & (elev_5 < ez),      # 0 上升陡坡形成（新起）
        up & (slope > act),                      # 1 上升坡延续
        up & (slope < -act),                     # 3 上升坡顶钝化
        up,                                      # 1 上升坡延续（中间带）
        dn & (slope < -act) & (elev_5 > -ez),    # 7 下降陡坡形成（新起）
        dn & (slope < -act),                     # 6 下降坡延续
        dn & (slope > act),                      # 5 下降谷底收敛
    ], [9, 2, 4, 8, 0, 1, 3, 1, 7, 6, 5], default=6)


def classify(ft, ez=ELEV_ZERO, sf=SLOPE_FRAC):
    """单日地貌名称（terrain_features 结果 → 分类树）。"""
    return CLASS_ORDER[int(classify_arr(
        np.array([ft["elev"]]), np.array([ft["slope_t"]]),
        np.array([ft["elev_5"]]), np.array([ft["coh"]]), ez, sf)[0])]


# ======================== 事件回测 ========================
def class_events(feat, ts, close, low, ez=ELEV_ZERO, sf=SLOPE_FRAC):
    """逐日分类 → 「首次进入地貌」事件 + 前瞻收益。

    事件取 class-run 首日且 run ≥ CONFIRM_DAYS（分类由平滑 EMA 驱动天然粘滞，
    取首日=信号触发日；确认窗滤掉分类边界 1 日翻覆；自动去重叠，避免相邻
    事件样本高度相关虚增显著性）。
    返回 (cls_code, ts, ret_5, ret_20, ret_65, mae_20) 列表，前瞻不足为 NaN。
    """
    valid = (np.isfinite(feat["elev"]) & np.isfinite(feat["slope_t"])
             & np.isfinite(feat["elev_5"]) & np.isfinite(feat["coh"]))
    cls = np.full(len(valid), -1, dtype=int)
    cls[valid] = classify_arr(feat["elev"][valid], feat["slope_t"][valid],
                              feat["elev_5"][valid], feat["coh"][valid], ez, sf)
    change = np.r_[True, cls[1:] != cls[:-1]]
    run_len = np.bincount(np.cumsum(change) - 1)   # 每个 run 的长度
    out = []
    c = close.to_numpy() if hasattr(close, "to_numpy") else np.asarray(close)
    lw = low.to_numpy() if hasattr(low, "to_numpy") else np.asarray(low)
    for pos, it in enumerate(np.nonzero(change)[0]):
        if not valid[it] or run_len[pos] < CONFIRM_DAYS:
            continue
        i = it + RATE_WIN                     # feat 为 RATE_WIN 截尾坐标, 还原全长索引
        rets = [c[i + p] / c[i] - 1.0 if i + p < len(c) else np.nan for p in FWD_PERIODS]
        mae = (lw[i + 1:i + 21].min() / c[i] - 1.0) if i + 1 < len(lw) else np.nan
        out.append((int(cls[it]), ts[i], *rets, mae))
    return out


def report_backtest(events, title):
    """事件回测汇总表: 各地貌前瞻收益均值/+65 胜率/+20 最大不利偏移。"""
    if not events:
        print("回看期内无地貌事件")
        return
    df = pd.DataFrame(events, columns=["cls", "ts", "ret_5", "ret_20", "ret_65", "mae_20"])
    n_days = df["ts"].nunique()
    print(f"\n{'='*96}")
    print(f" 地貌事件回测 [{title}] 事件=首次进入地貌日 | 样本 {len(df)} 事件 / {n_days} 个覆盖交易日")
    print(f" 前瞻收益按信号日收盘（前复权含分红）; 参考成本: 佣金+印花税往返 ≈0.15%")
    print(f"{'='*96}")
    print(f"{'地貌':<10} {'样本':>6} {'覆盖日':>6} {'+5日均':>8} {'+20日均':>8} "
          f"{'+65日均':>8} {'+65胜率':>8} {'+20MAE':>8}")
    print("-" * 96)
    for code, name in enumerate(CLASS_ORDER):
        g = df[df["cls"] == code]
        if g.empty:
            continue
        line = f"{name:<10} {len(g):>6} {g['ts'].nunique():>6}"
        for p in FWD_PERIODS:
            v = g[f"ret_{p}"].dropna()
            line += f" {v.mean():>7.1%}" if len(v) else "      --"
        v65 = g["ret_65"].dropna()
        line += f" {(v65 > 0).mean():>8.1%}" if len(v65) else "       --"
        vm = g["mae_20"].dropna()
        line += f" {vm.mean():>7.1%}" if len(vm) else "      --"
        print(line)
    line = f"{'全体事件':<10} {len(df):>6} {n_days:>6}"
    for p in FWD_PERIODS:
        line += f" {df[f'ret_{p}'].dropna().mean():>7.1%}"
    v65 = df["ret_65"].dropna()
    line += f" {(v65 > 0).mean():>8.1%} {df['mae_20'].dropna().mean():>7.1%}"
    print("-" * 96)
    print(line)
    print(f"{'='*96}")
    print("读法: 上升类地貌 +65 均值/胜率须显著高于全体才有选股价值; "
          "MAE 为 +20 日内最大不利偏移（止损设计参考）", file=sys.stderr)
    return df


def run_calibration(store, title):
    """阈值走步标定: (ELEV_ZERO×SLOPE_FRAC) 网格, 内样本 70% 选参 → 外样本验证。

    选参指标: 「上升陡坡形成」+20 日超额（对全体事件均值）——选股视角最直接的
    分离度。ponytail: 单分裂走步（一段 IS/一段 OOS），滚动多窗版本待需要再加。
    """
    all_ts = np.concatenate([s["ts"] for s in store]).astype("datetime64[ns]")
    split = np.datetime64(int(np.quantile(all_ts.astype("int64"), IS_FRAC)), "ns")
    rows = []
    for ez in CAL_EZ:
        for sf in CAL_SF:
            ev = []
            for s in store:
                ev += [e for e in class_events(s["feat"], s["ts"], s["close"], s["low"], ez, sf)]
            df = pd.DataFrame(ev, columns=["cls", "ts", "ret_5", "ret_20", "ret_65", "mae_20"])
            is_df, oos_df = df[df["ts"] < split], df[df["ts"] >= split]

            def excess(d):
                g = d[(d["cls"] == 0) & d["ret_20"].notna()]
                base = d["ret_20"].dropna()
                if len(g) < CAL_MIN_N or base.empty:
                    return None, None, len(g)
                return g["ret_20"].mean() - base.mean(), (g["ret_20"] > 0).mean(), len(g)

            ex_is, wr_is, n_is = excess(is_df)
            ex_oos, wr_oos, n_oos = excess(oos_df)
            rows.append(dict(ez=ez, sf=sf, n_is=n_is, ex_is=ex_is, wr_is=wr_is,
                             n_oos=n_oos, ex_oos=ex_oos, wr_oos=wr_oos))
    print(f"\n{'='*96}")
    print(f" 阈值走步标定 [{title}] 内样本<={str(split)[:10]} ({IS_FRAC:.0%}) / 外样本>该日")
    print(f" 指标: 「上升陡坡形成」+20 日超额收益（对全体事件; 每组合 IS 事件≥{CAL_MIN_N}）")
    print(f"{'='*96}")
    print(f"{'ELEV_ZERO':>9} {'SLOPE_FRAC':>10} | {'IS样本':>7} {'IS超额':>8} {'IS胜率':>7} "
          f"| {'OOS样本':>7} {'OOS超额':>8} {'OOS胜率':>7}")
    print("-" * 96)
    ok = [r for r in rows if r["ex_is"] is not None]
    for r in rows:
        if r["ex_is"] is None:
            print(f"{r['ez']:>9} {r['sf']:>10} | {'样本不足':>34}")
            continue
        print(f"{r['ez']:>9} {r['sf']:>10} | {r['n_is']:>7} {r['ex_is']:>8.1%} {r['wr_is']:>7.1%} "
              f"| {r['n_oos']:>7} {r['ex_oos'] if r['ex_oos'] is not None else float('nan'):>8.1%} "
              f"{r['wr_oos'] if r['wr_oos'] is not None else float('nan'):>7.1%}")
    print("-" * 96)
    if ok:
        best = max(ok, key=lambda r: r["ex_is"])
        print(f" 选中: ELEV_ZERO={best['ez']}, SLOPE_FRAC={best['sf']} "
              f"(IS 超额 {best['ex_is']:+.1%})")
        if best["ex_oos"] is None:
            print(" 外样本样本不足，无法验证", file=sys.stderr)
        else:
            keep = best["ex_oos"] > 0
            print(f" 外样本超额 {best['ex_oos']:+.1%} → {'通过（未过拟合迹象）' if keep else '未通过（IS 过拟合嫌疑，谨慎使用）'}")
    print(f"{'='*96}", file=sys.stderr)
    return rows


# ======================== 自检 ========================
def self_test():
    rng = np.random.default_rng(42)

    def ohl(closes):                             # high/low 各让 2 分
        c = pd.Series(closes)
        return c, c + 0.02, c - 0.02

    # 1) 持续线性上升 → 上升坡延续/形成，高相干
    up = 10 + 0.08 * np.arange(250) + rng.normal(0, 0.02, 250)
    ft = terrain_features(*ohl(up))
    cls = classify(ft)
    assert cls in ("上升坡延续", "上升陡坡形成"), (cls, ft)
    assert ft["coh"] > 0.9 and ft["elev"] > 0.3, ft

    # 2) 持续线性下降 → 下降坡延续，高程为负
    dn = 30 - 0.10 * np.arange(250) + rng.normal(0, 0.02, 250)
    ft = terrain_features(*ohl(dn))
    cls = classify(ft)
    assert cls in ("下降坡延续", "下降陡坡形成"), (cls, ft)
    assert ft["coh"] > 0.9 and ft["elev"] < -0.3, ft

    # 3) 长期横盘 → 零区（转换区/转折带/杂波），高程≈0
    flat = 10 + rng.normal(0, 0.03, 250)
    ft = terrain_features(*ohl(flat))
    assert abs(ft["elev"]) < ELEV_ZERO, ft
    assert classify(ft) in ("地形转换区", "向上转折带", "向下转折带", "断裂杂波地形"), ft

    # 4) 先跌后拉起（V 形右半）→ 向上转折带或上升陡坡形成
    v = np.concatenate([20 - 0.15 * np.arange(150), 2 + 0.15 * np.arange(100)])
    ft = terrain_features(*ohl(v))
    assert classify(ft) in ("向上转折带", "上升陡坡形成", "上升坡延续"), ft

    # 5) 向量版与标量版同口径: terrain_series 末日特征/分类 == terrain_features
    ft_up = terrain_features(*ohl(up))           # 重算（ft 此刻是 V 形的）
    sr = terrain_series(*ohl(up))
    last = {k: arr[-1] for k, arr in sr.items()}
    assert abs(last["elev"] - ft_up["elev"]) < 1e-9, (last, ft_up)
    assert abs(last["coh"] - ft_up["coh"]) < 1e-9, (last, ft_up)
    assert CLASS_ORDER[classify_arr(sr["elev"][-1:], sr["slope_t"][-1:],
                                    sr["elev_5"][-1:], sr["coh"][-1:])[0]] \
        == classify(ft_up)

    # 6) 事件回测机制: 首事件 ret_20 与全长 close 精确对位（验 RATE_WIN 坐标还原）
    #    合成坡高程 ~14 ATR 病态（噪声/ATR 失真），延续/钝化边界多日翻覆属预期，
    #    只验对位与事件类型，不验分类稳定性
    ts = pd.date_range("2025-01-01", periods=250, freq="B").values
    c = pd.Series(up)
    ev = class_events(sr, ts, c, c - 0.02)
    assert len(ev) >= 1 and all(e[0] in (0, 1, 3) for e in ev), ev   # 上升坡只出上升类
    e0 = ev[0]
    v0 = (np.isfinite(sr["elev"]) & np.isfinite(sr["elev_5"])
          & np.isfinite(sr["slope_t"]) & np.isfinite(sr["coh"]))
    i0 = int(np.nonzero(v0)[0][0]) + RATE_WIN   # 首个 valid 位, 还原全长索引
    assert abs(e0[3] - (up[i0 + 20] / up[i0] - 1)) < 1e-9, e0
    # MAE: 单调上升坡上未来低点也不会低于信号日收盘 → 0 ≤ mae_20 < ret_20
    assert all(0 <= e[5] < e[3] + 1e-9 for e in ev
               if np.isfinite(e[5]) and np.isfinite(e[3])), ev

    # 7) V 形: 至少两个事件（下降类 + 上升/转折类）
    sr_v = terrain_series(*ohl(v))
    ev_v = class_events(sr_v, ts, pd.Series(v), pd.Series(v) - 0.02)
    assert len(ev_v) >= 2 and len({e[0] for e in ev_v}) >= 2, ev_v
    print("self-test OK")


# ======================== 主函数 ========================
def main():
    parser = argparse.ArgumentParser(description="相控阵地形测绘选股（诊断/回测/标定）")
    parser.add_argument("--all", action="store_true", help="全 A 股 (默认仅自选股)")
    parser.add_argument("--days", type=int, default=None,
                        help="回看交易日 (默认: 当日模式 160 / 回测标定 500)")
    parser.add_argument("--top", type=int, default=15, help="当日模式每类屏显条数 (默认 15)")
    parser.add_argument("--backtest", action="store_true", help="事件回测: 地貌→前瞻收益分布")
    parser.add_argument("--calibrate", action="store_true",
                        help="阈值走步标定 (ELEV_ZERO×SLOPE_FRAC 网格 + IS/OOS)")
    parser.add_argument("--self-test", action="store_true", help="运行内置自检后退出")
    args = parser.parse_args()

    if args.self_test:
        self_test()
        return 0

    days = args.days or (BACKTEST_DAYS if (args.backtest or args.calibrate) else LOOKBACK_DAYS)
    conn = taosws.connect(TDENGINE_URL)
    cursor = conn.cursor()
    names_by_code = load_stock_names(conn)

    t0 = time.time()
    today_str = time.strftime("%Y-%m-%d")
    db_codes = set(get_a_stock_codes(cursor))
    if args.all:
        pool = set(all_mainboard_codes(conn))
        pool_desc = f"全 A 股 {len(pool)} 只"
    else:
        pool = {parse_code(c) for c in zxg_codes()}
        pool_desc = f"自选股 zxg.blk {len(pool)} 只"
    a_stocks = sorted(db_codes & pool)
    if not a_stocks:
        sys.stderr.write("无候选标的（加 --all 筛全市场）\n")
        return 1
    adj_by_mc = batch_fetch_adjust(conn, a_stocks)

    start_date = (datetime.now() - timedelta(days=days * 3 // 2 + 70)).strftime("%Y-%m-%d")
    print(f"[1/3] {pool_desc} ∩ DB = {len(a_stocks)} 只, 回看 {days} 交易日", file=sys.stderr)

    frames = iter_stock_frames(cursor, a_stocks, adj_by_mc, start_date, today_str)

    if not (args.backtest or args.calibrate):
        # -------- 当日诊断模式 --------
        results = []
        for code, df in frames:
            ft = terrain_features(df["C"].astype(float), df["H"].astype(float),
                                  df["L"].astype(float))
            if ft is None:
                continue
            results.append((code, classify(ft), ft, float(df["C"].iloc[-1])))
        conn.close()
        if not results:
            print("无有效标的")
            return 0
        df = pd.DataFrame([{
            "代码": c, "名称": names_by_code.get(c, ""), "现价": round(close, 2),
            "地貌": cls, "高程": round(ft["elev"], 2), "时向坡度": round(ft["slope_t"], 3),
            "相干度": round(ft["coh"], 2), "主波束°": round(ft["beam"], 0),
            "截面坡度": round(ft["cross"], 2),
        } for c, cls, ft, close in results])
        df["_prio"] = df["地貌"].map({c: i for i, c in enumerate(DISPLAY_ORDER)})
        df = df.sort_values(["_prio", "相干度"], ascending=[True, False]).drop(columns="_prio")

        print(f"\n{'='*96}")
        print(f" 相控阵地貌分布 [{pool_desc}] 数据截至 {today_str}（前复权）")
        print(f"{'='*96}")
        for cls in DISPLAY_ORDER:
            n = (df["地貌"] == cls).sum()
            if n:
                print(f"  {cls:<8} {n:>5} 只")
        cols = ["代码", "名称", "现价", "地貌", "高程", "时向坡度", "相干度", "主波束°", "截面坡度"]
        # 反转漏斗类先出（策略定位）, 动量类殿后（仅诊断）
        for cls in ("下降谷底收敛", "向上转折带", "向下转折带", "上升坡顶钝化"):
            g = df[df["地貌"] == cls].head(args.top)
            if g.empty:
                continue
            print(f"\n--- {cls} (按相干度降序, 最多 {args.top} 只) ---")
            print("  ".join(cols))
            for _, x in g.iterrows():
                print("  ".join(str(x[c]) for c in cols))
        print(f"{'='*96}")
        print("阈值自设未经回测标定，仅供诊断；高相干度≠独立共识（六阵元同源一条价格）",
              file=sys.stderr)
        out_dir = os.path.join(OUTPUT_DIR, "find-terrain")
        os.makedirs(out_dir, exist_ok=True)
        stamped = os.path.join(out_dir, f"find-terrain-{time.strftime('%Y%m%d')}.xlsx")
        df.to_excel(stamped, index=False)
        print(f"[xlsx] → {stamped} (共 {len(df)} 条)", file=sys.stderr)
        return 0

    # -------- 回测/标定模式: 逐只全历史特征 --------
    store = []      # {code, ts, close, low, feat}
    for code, df in frames:
        feat = terrain_series(df["C"].astype(float), df["H"].astype(float),
                              df["L"].astype(float))
        if feat is None:
            continue
        store.append(dict(code=code, ts=df["ts"].values,
                          close=df["C"].values, low=df["L"].values, feat=feat))
    conn.close()
    if not store:
        print("无有效标的")
        return 0
    d_min = min(s["ts"][0] for s in store)
    d_max = max(s["ts"][-1] for s in store)
    title = f"{pool_desc} · {str(d_min)[:7]}~{str(d_max)[:7]}"

    if args.backtest:
        events = []
        for s in store:
            events += class_events(s["feat"], s["ts"], s["close"], s["low"])
        ev_df = report_backtest(events, title)
        if ev_df is not None:
            out_dir = os.path.join(OUTPUT_DIR, "find-terrain")
            os.makedirs(out_dir, exist_ok=True)
            stamped = os.path.join(out_dir, f"backtest-events-{time.strftime('%Y%m%d')}.csv")
            ev_df.to_csv(stamped, index=False)
            print(f"[csv] → {stamped} (共 {len(ev_df)} 事件)", file=sys.stderr)

    if args.calibrate:
        run_calibration(store, title)
    return 0


if __name__ == "__main__":
    sys.exit(main())
