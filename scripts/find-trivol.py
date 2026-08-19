#!/usr/bin/env python3
"""
================================================================================
 寻一「三倍量」战法选股 — 三倍量信号 + 红线金叉 + 回调低吸（诊断 + 回测）
================================================================================

源自头条博主「寻一（无小不私聊）」全部 705 篇微头条（2025-10 ~ 2026-08，原文
存 docs/xunyi_posts.md，其中 270 处提及三倍量）。方法规则均为博主原文：

  信号    三倍量日 = V ≥ 前一日×3 且收阳上涨（博主主图公式逐字:
          「三倍量:=V>=REF(V,1)*3 AND C>REF(C,1) AND C>O」，
          STICKLINE 洋红标记 + DRAWSL 沿三倍量收盘价画红线长期延伸）
          有效硬标准（博主「四个硬性标准」帖）: 当日量 ≥ 近5日均量×3，
          3~5 倍最佳；超 10 倍巨量排除（主力对倒诱多），低于 3 倍太弱
  红线    = 三倍量日收盘价。案例中「三倍量出现两次，冲击上线失败，回踩下线
          企稳」「回调到第二个三倍量收盘价」——红线数月内持续有效
  均线    EXPMA5（上线）/ EXPMA12（下线）:「两条均线之间都是买点」
          「只要不跌破12日均线，应该都可以持续买入」「十二的均线向上最好」
  买点    ① 金叉(最强):「均线上穿三倍量收盘价」「当天三倍量，二至三天形成
            金叉应该是最强的」「形成金叉没有大涨的股票」
          ② 回调低吸: 三倍量后「缩量回踩」，价进入两线区间或回踩红线，
            「回调不破放量当天的最低价」「量能必须萎缩」
          ③ 突破:「突破了三倍量收盘价买入」；三倍量次日缩量站上红线尾盘
            买入（帖136）；次日跌破 = 假突破（博主逢低补）
  出场    「止损价也为三倍量收盘价」（红线）；「跌破放量K线的最低点果断止损」；
          「一直等到涨停就卖掉」不涨停冲高卖出；跌破 EXPMA12 警惕
  排除    博主原话「非科创板非北交所非ST」→ 剔除 688/689、bj、ST

三种模式:
  默认        当日候选屏显 + xlsx（前置: fetch-today 已跑，当日 bar 已入库）
  --backtest  事件回测: 三类买点 → 规则出场（止损=红线/破量低、涨停卖出、
              20 交易日强制）收益分布，按 250 日位置高低分组验证「低位优先」
  --self-test 合成数据自检

用法:
  python3 scripts/find-trivol.py                    # 自选股
  python3 scripts/find-trivol.py --all              # 全 A 股
  python3 scripts/find-trivol.py --all --backtest
  python3 scripts/find-trivol.py --self-test
================================================================================
"""

import argparse
import os
import sys
import time
from datetime import datetime, timedelta
from decimal import Decimal, ROUND_HALF_UP

import numpy as np
import pandas as pd
from common import (OUTPUT_DIR, all_mainboard_codes, apply_qfq, batch_fetch_adjust,
                    is_st_name, load_stock_names, market_line,
                    market_regime, parse_code, zxg_codes, connect)

# ======================== 配置 ========================
BATCH_SIZE = 1800
MIN_ROWS = 70            # 5日均量 + EXPMA12 + 250日位置所需最少数据
LOOKBACK_DAYS = 320      # 当日模式回看交易日（250 日位置 + 红线窗口余量）
BACKTEST_DAYS = 550      # 回测回看交易日
ANCHOR_WINDOW = 60       # 三倍量后 N 交易日内买点有效（红线 DRAWSL 1260 根延伸，取时效收敛值）
CROSS_FRESH = 3          # 金叉「新鲜」窗口: EXPMA12 上穿红线后 N 日内且未大涨
CROSS_MAX_EXTEND = 0.15  # 金叉日现价距红线上限（「形成金叉没有大涨」）
MAX_HOLD = 20            # 回测最大持有交易日（「死等大涨卖出」的耐心上限）
POS_SPLIT = 0.6          # 250 日位置分组阈值（<0.6 低位 / ≥0.6 高位）

VOL_MULT_MIN = 3.0       # 量能倍数下限（对前一日 & 近5日均量同时成立）
VOL_MULT_MAX = 10.0      # 近5日均量倍数上限（>10 排除，对倒诱多）

STATE_ORDER = ["金叉", "回调低吸", "突破", "持有", "观望"]
ENTRY_NAMES = {0: "金叉", 1: "回调低吸", 2: "突破"}   # 回测事件类型代码


# ======================== 涨停判定（对齐 find-retrace.py） ========================
def limit_ratio(market, num, name):
    """涨停幅度: 科创/创业板 20% / 主板 10% / 主板 ST 5%（bj 已在标的池剔除）。"""
    if num.startswith("688") or num.startswith("689") or num.startswith("30"):
        return 0.20
    return 0.05 if is_st_name(name) else 0.10


def limit_up_price(prev_close, ratio):
    """涨停价 = 前收 ×(1+比率)，四舍五入到分（Decimal 防浮点错分）。"""
    p = (Decimal(str(prev_close)) * (1 + Decimal(str(ratio)))).quantize(
        Decimal("0.01"), rounding=ROUND_HALF_UP)
    return float(p)


def is_limit_up(close, prev_close, market, num, name):
    """收盘是否封住涨停价（价格均 2 位小数，差 <0.005 即相等）。"""
    limit = limit_up_price(prev_close, limit_ratio(market, num, name))
    return abs(close - limit) < 0.005


# ======================== 数据获取（对齐 find-terrain.py 惯例） ========================
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
        f"SELECT ts, open, high, low, close, volume, '{market}{num}' as code "
        f"FROM tdx.k_{market}{num}_1d WHERE ts >= '{start_date}'"
        for market, num in codes
    ]
    cursor.execute(" UNION ALL ".join(queries) + " ORDER BY code, ts")
    return cursor.fetchall()


def is_excluded(market, num, name):
    """博主原话「非科创板非北交所非ST」。"""
    return market == "bj" or num.startswith(("688", "689")) or is_st_name(name)


def iter_stock_frames(cursor, codes, adj_by_mc, start_date):
    """批量拉日线（含 volume）→ 逐只 yield (code, qfq DataFrame[ts,O,H,L,C,V])。

    保留当日 bar（尾盘选股依赖当日现价，前置 fetch-today 已入库）。
    """
    total_batches = (len(codes) + BATCH_SIZE - 1) // BATCH_SIZE
    for bi in range(total_batches):
        t2 = time.time()
        batch = codes[bi * BATCH_SIZE:(bi + 1) * BATCH_SIZE]
        by_code = {}
        for row in batch_query_kline(cursor, batch, start_date):
            by_code.setdefault(row[6], []).append(row)
        n_ok = 0
        for market, num in batch:
            rows = by_code.get(f"{market}{num}", [])
            if len(rows) < MIN_ROWS:
                continue
            df = pd.DataFrame([r[:6] for r in rows], columns=["ts", "O", "H", "L", "C", "V"])
            df["ts"] = pd.to_datetime(df["ts"])
            events = adj_by_mc.get((market, num))
            if events:
                df = apply_qfq(df, events)
            if (df[["O", "H", "L", "C"]] <= 0).any().any():
                continue
            n_ok += 1
            yield f"{market}{num}", df
        print(f"[2/3] 批次 {bi+1}/{total_batches}: {time.time()-t2:.1f}s, 有效 {n_ok}/{len(batch)}",
              file=sys.stderr)


# ======================== 三倍量核心 ========================
def find_anchors(df):
    """三倍量日下标数组。

    博主主图公式 V>=REF(V,1)*3 AND C>REF(C,1) AND C>O，
    叠加「有效三倍量」硬标准: V/近5日均量（不含当日）∈ [3, 10]。
    """
    v = df["V"].astype(float)
    ratio1 = v / v.shift(1)
    ratio5 = v / v.shift(1).rolling(5).mean()
    ok = (ratio1 >= VOL_MULT_MIN) & (ratio5 >= VOL_MULT_MIN) & (ratio5 <= VOL_MULT_MAX) \
        & (df["C"] > df["C"].shift(1)) & (df["C"] > df["O"])
    ok = ok.fillna(False)
    return np.flatnonzero(ok.to_numpy())


def add_expma(df):
    """EXPMA5/12（对齐通达信 EXPMA = pandas ewm span, adjust=False）。"""
    df = df.copy()
    df["E5"] = df["C"].ewm(span=5, adjust=False).mean()
    df["E12"] = df["C"].ewm(span=12, adjust=False).mean()
    df["pos250"] = df["C"] / df["C"].rolling(250).max()   # 250 日位置（低位诊断）
    return df


def entries_for_anchor(df, a):
    """单个三倍量锚 → 三类买点触发下标 {0:金叉, 1:回调低吸, 2:突破}（各取首个触发日）。

    窗口 [a, a+ANCHOR_WINDOW]。红线 = C[a]，量低 = L[a]。
      金叉      E12 上穿红线（前日 ≤ 红线，当日 >），且当日未大涨（C ≤ 红线×1.15）
      回调低吸  缩量（V < 三倍量日 V）且当日低点触及两线区间或红线，
                收盘未破量低（「回调不破放量当天的最低价」）
      突破      前收 ≤ 红线，当日收盘 > 红线; 首日（三倍量次日）须缩量站上
    """
    n = len(df)
    end = min(a + 1 + ANCHOR_WINDOW, n)
    red = float(df["C"].iloc[a])
    v_anchor = float(df["V"].iloc[a])
    low_anchor = float(df["L"].iloc[a])
    out = {}
    if end <= a + 1:
        return out
    seg = df.iloc[a + 1:end]
    e12_up = seg["E12"] > seg["E12"].shift(1)
    # 金叉: E12 上穿红线
    cross = (seg["E12"] > red) & (seg["E12"].shift(1) <= red) & e12_up \
        & (seg["C"] <= red * (1 + CROSS_MAX_EXTEND))
    if cross.any():
        out[0] = a + 1 + int(np.flatnonzero(cross.to_numpy())[0])
    # 回调低吸: 缩量 + 触及两线区间或红线 + 未破量低
    zone_hi = pd.concat([seg["E5"], seg["E12"]], axis=1).max(axis=1)
    zone_lo = pd.concat([seg["E5"], seg["E12"]], axis=1).min(axis=1)
    touch = (seg["L"] <= zone_hi) | (seg["L"] <= red * 1.02)
    pullback = (seg["V"] < v_anchor) & touch & (seg["C"] >= low_anchor) \
        & (seg["C"] <= zone_hi * 1.02)
    if pullback.any():
        out[1] = a + 1 + int(np.flatnonzero(pullback.to_numpy())[0])
    # 突破: 收盘上穿红线。首日前收 = 三倍量日收盘 = 红线，首日站上即帖136
    # 「三倍量次日缩量站上三倍量收盘价尾盘买入」→ 首日须缩量，防放量冲高误报
    prev_c = np.append(red, seg["C"].to_numpy()[:-1])
    brk = (seg["C"].to_numpy() > red) & (prev_c <= red)
    brk[0] &= float(seg["V"].iloc[0]) < v_anchor
    if brk.any():
        out[2] = a + 1 + int(np.flatnonzero(brk)[0])
    return out


def simulate(df, i, red, low_anchor, market, num, name, use_red_stop=True):
    """规则出场模拟: 买入价 = 信号日收盘。

    止损分买点（对齐博主原文）: 金叉/突破买入价在红线上方 → 止损=红线（帖26
    「止损价也为三倍量收盘价」）+ 破量低兜底; 回调低吸买入价常在红线下方 →
    止损=量低（帖112「跌破放量K线的最低点果断止损」），红线止损不适用。
    其余出场: 涨停卖出（「一直等到涨停就卖掉」）> MAX_HOLD 强制。
    返回 (收益, 持有交易日, 出场原因)。
    """
    c = df["C"].to_numpy()
    n = len(df)
    buy = float(c[i])
    for j in range(i + 1, min(i + 1 + MAX_HOLD, n)):
        if c[j] < low_anchor:
            return c[j] / buy - 1, j - i, "破量低"
        if use_red_stop and c[j] < red:
            return c[j] / buy - 1, j - i, "止损红线"
        if is_limit_up(c[j], c[j - 1], market, num, name):
            return c[j] / buy - 1, j - i, "涨停"
    j = min(i + MAX_HOLD, n - 1)
    return float(c[j]) / buy - 1, j - i, "到期"


# ======================== 当日候选分类 ========================
def classify_state(df, a, today_i):
    """最新交易日相对最近三倍量锚的状态（对齐博主买点表述）。"""
    t = df.iloc[today_i]
    red = float(df["C"].iloc[a])
    low_anchor = float(df["L"].iloc[a])
    v_anchor = float(df["V"].iloc[a])
    c, e5, e12 = float(t["C"]), float(t["E5"]), float(t["E12"])
    age = today_i - a
    if age > ANCHOR_WINDOW:
        return "观望", red, age
    if c < low_anchor or c < e12 * 0.99:
        return "破位", red, age      # 已破位，不入候选
    zone_hi, zone_lo = max(e5, e12), min(e5, e12)
    # 金叉: E12 近 CROSS_FRESH 日内上穿红线且现价未大涨
    e12_series = df["E12"].iloc[max(a, today_i - CROSS_FRESH):today_i + 1]
    crossed = (e12_series > red) & (e12_series.shift(1) <= red)
    if crossed.any() and c <= red * (1 + CROSS_MAX_EXTEND) and e12 > df["E12"].iloc[today_i - 1]:
        return "金叉", red, age
    # 突破: 前收 ≤ 红线 < 当日收盘。锚龄=1 时前收 = 锚收 = 红线，须缩量站上
    # （帖136「三倍量次日缩量站上三倍量收盘价尾盘买入」）
    if age >= 1 and df["C"].iloc[today_i - 1] <= red < c \
            and (age > 1 or float(t["V"]) < v_anchor):
        return "突破", red, age
    shrink = float(t["V"]) < v_anchor
    if shrink and (float(t["L"]) <= zone_hi or float(t["L"]) <= red * 1.02) and c >= low_anchor:
        return "回调低吸", red, age
    if c > red and c > zone_hi:
        return "持有", red, age      # 已过买点，持有区（不破 12 线可持有）
    return "观望", red, age


# ======================== 回测 ========================
def run_backtest(frames_meta, title):
    """三类买点事件回测: 规则出场收益 + 原始前瞻收益，按 250 日位置分组。

    frames_meta: [(code, market, num, name, df), ...]
    """
    events = []   # (类型, code, ts, sim_ret, hold, reason, ret5, ret20, mae20, pos250)
    for code, market, num, name, df in frames_meta:
        c = df["C"].to_numpy()
        lw = df["L"].to_numpy()
        n = len(df)
        for a in find_anchors(df):
            red = float(c[a])
            low_anchor = float(lw[a])
            for etype, i in entries_for_anchor(df, a).items():
                ret5 = c[i + 5] / c[i] - 1 if i + 5 < n else np.nan
                ret20 = c[i + 20] / c[i] - 1 if i + 20 < n else np.nan
                mae = lw[i + 1:i + 21].min() / c[i] - 1 if i + 1 < min(i + 21, n) else np.nan
                sim, hold, reason = simulate(df, i, red, low_anchor, market, num, name,
                                             use_red_stop=(etype != 1))
                events.append((etype, code, str(df["ts"].iloc[i])[:10], sim, hold, reason,
                               ret5, ret20, mae, float(df["pos250"].iloc[i])))
    if not events:
        print("回看期内无三倍量买点事件")
        return None
    df_ev = pd.DataFrame(events, columns=["类型", "代码", "ts", "sim_ret", "持有日",
                                          "出场", "ret_5", "ret_20", "mae_20", "pos250"])
    print(f"\n{'='*100}")
    print(f" 三倍量买点事件回测 [{title}] | 出场: 金叉/突破止损=红线, 回调低吸止损=量低; 涨停卖出 → {MAX_HOLD} 日强制")
    print(f" 事件=每锚每类首个触发日; 成本参考: 佣金+印花税往返 ≈0.15%（未计入）")
    print(f"{'='*100}")
    print(f"{'买点类型':<8} {'分组':<4} {'样本':>6} {'规则收益':>8} {'胜率':>6} {'均持日':>6} "
          f"{'+5日均':>8} {'+20日均':>8} {'+20MAE':>8}")
    print("-" * 100)
    csv_rows = []
    for etype, ename in ENTRY_NAMES.items():
        g0 = df_ev[df_ev["类型"] == etype]
        for grp, g in (("全体", g0), ("低位", g0[g0["pos250"] < POS_SPLIT]),
                       ("高位", g0[g0["pos250"] >= POS_SPLIT])):
            if g.empty:
                continue
            line = f"{ename:<8} {grp:<4} {len(g):>6}"
            line += f" {g['sim_ret'].mean():>8.1%} {(g['sim_ret'] > 0).mean():>6.1%}"
            line += f" {g['持有日'].mean():>6.1f}"
            for col in ("ret_5", "ret_20", "mae_20"):
                v = g[col].dropna()
                line += f" {v.mean():>8.1%}" if len(v) else "      --"
            print(line)
            csv_rows.append(dict(类型=ename, 分组=grp, 样本=len(g),
                                 规则收益=g["sim_ret"].mean(),
                                 胜率=(g["sim_ret"] > 0).mean(),
                                 均持日=g["持有日"].mean(),
                                 ret_5=g["ret_5"].dropna().mean(),
                                 ret_20=g["ret_20"].dropna().mean(),
                                 mae_20=g["mae_20"].dropna().mean()))
        rc = g0["出场"].value_counts()
        print(f"         出场: " + "  ".join(f"{k}×{v}" for k, v in rc.items()))
    print(f"{'='*100}")
    print("读法: 「金叉」为博主认定的最强买点，其规则收益/胜率应显著优于「突破」（追高）;\n"
          f"      低位组 vs 高位组验证「警惕高位放量出货」（分组阈值 pos250<{POS_SPLIT}）",
          file=sys.stderr)
    return df_ev


# ======================== 自检 ========================
def _mk_df(o, h, l, c, v, start="2025-01-01"):
    ts = pd.date_range(start, periods=len(c), freq="B")
    return pd.DataFrame({"ts": ts, "O": o, "H": h, "L": l, "C": c, "V": v})\
        .astype({"O": float, "H": float, "L": float, "C": float, "V": float})


def self_test():
    rng = np.random.default_rng(7)

    # 1) 三倍量识别: 横盘 → 放量 3.5 倍阳线 → 命中; >10 倍 / <3 倍 / 阴线 / 下跌 均不命中
    base = 10 + rng.normal(0, 0.05, 80)
    v = np.full(80, 1_000_000.0)
    c = base.copy(); o = c - 0.03; h = c + 0.05; l = c - 0.08
    c[50] = c[49] + 0.5; o[50] = c[50] - 0.2; h[50] = c[50] + 0.1; l[50] = c[50] - 0.3
    v[50] = 3_500_000.0                                  # 3.5 倍于前日均量 → 有效
    df = add_expma(_mk_df(o, h, l, c, v))
    anch = find_anchors(df)
    assert list(anch) == [50], anch

    v2 = v.copy(); v2[50] = 11_000_000.0                 # >10 倍 → 排除
    assert list(find_anchors(add_expma(_mk_df(o, h, l, c, v2)))) == []
    v3 = v.copy(); v3[50] = 2_900_000.0                  # <3 倍 → 排除
    assert list(find_anchors(add_expma(_mk_df(o, h, l, c, v3)))) == []
    c4 = c.copy(); c4[50] = c4[50] - 0.3                 # 改阴线（C<O, 仍>前收）→ 排除
    assert list(find_anchors(add_expma(_mk_df(o, h, l, c4, v)))) == []
    c5 = c.copy(); c5[49] = c5[50] + 0.1                 # 当日收跌（C<REF(C,1)）→ 排除
    assert list(find_anchors(add_expma(_mk_df(o, h, l, c5, v)))) == []

    # 2) 买点触发: 三倍量后回调进两线区间 → 回调低吸; 反弹上穿红线 → 突破
    c2 = base.copy(); o2 = c2 - 0.03; h2 = c2 + 0.05; l2 = c2 - 0.08
    v2 = np.full(80, 1_000_000.0)
    c2[50] = c2[49] + 0.5; o2[50] = c2[50] - 0.2; h2[50] = c2[50] + 0.1; l2[50] = c2[50] - 0.3
    v2[50] = 3_500_000.0
    # 51: 冲高回落阴线（缩量）; 52: 回落到两线区间; 53: 站上红线（突破）
    c2[51] = c2[50] + 0.05; o2[51] = c2[51] + 0.1; h2[51] = c2[50] + 0.2; l2[51] = c2[51] - 0.2
    c2[52] = c2[50] - 0.15; o2[52] = c2[52] - 0.02; h2[52] = c2[52] + 0.1; l2[52] = c2[52] - 0.1
    c2[53] = c2[50] + 0.1; o2[53] = c2[53] - 0.05; h2[53] = c2[53] + 0.1; l2[53] = c2[53] - 0.1
    df2 = add_expma(_mk_df(o2, h2, l2, c2, v2))
    a = find_anchors(df2)[0]
    ent = entries_for_anchor(df2, a)
    assert ent.get(1) is not None and ent[1] in (51, 52), ent      # 回调低吸在 51/52
    assert ent.get(2) == 51, ent   # 51 缩量站上红线 → 首日突破（帖136）
    # 首日突破须缩量: 51 放量站上 → 不算，首个突破顺延到 53（回调后再上穿）
    v2b = v2.copy(); v2b[51] = 4_000_000.0
    df2b = add_expma(_mk_df(o2, h2, l2, c2, v2b))
    ent_b = entries_for_anchor(df2b, find_anchors(df2b)[0])
    assert ent_b.get(2) == 53, ent_b
    # 3) 状态分类: 52 日当天 → 回调低吸; 53 日当天 → 突破或持有
    st, red, age = classify_state(df2, a, 52)
    assert st == "回调低吸", st
    st, _, _ = classify_state(df2, a, 53)
    assert st in ("突破", "持有", "金叉"), st
    # 4) 出场模拟: 买入后跌破红线 → 止损; 涨停 → 卖出
    c3 = base.copy(); o3 = c3 - 0.03; h3 = c3 + 0.05; l3 = c3 - 0.08
    v3 = np.full(80, 1_000_000.0)
    c3[50] = c3[49] + 0.5; o3[50] = c3[50] - 0.2; h3[50] = c3[50] + 0.1; l3[50] = c3[50] - 0.3
    v3[50] = 3_500_000.0
    c3[51] = c3[50] * 0.9                                # 次日大跌破红线
    o3[51] = c3[51] + 0.1; h3[51] = c3[51] + 0.1; l3[51] = c3[51] - 0.1
    df3 = add_expma(_mk_df(o3, h3, l3, c3, v3))
    ret, hold, reason = simulate(df3, 50, float(df3["C"].iloc[50]), float(df3["L"].iloc[50]),
                                 "sz", "000001", "平安银行")
    assert reason in ("止损红线", "破量低") and ret < 0, (ret, hold, reason)
    # 涨停出场: 主板 10%（9.9 → 10.89 涨停）
    assert is_limit_up(10.89, 9.9, "sz", "000001", "平安银行")
    assert not is_limit_up(10.88, 9.9, "sz", "000001", "平安银行")
    # 创业板 20%
    assert is_limit_up(11.88, 9.9, "sz", "300001", "XX")
    print("self-test OK")


# ======================== 主函数 ========================
def main():
    parser = argparse.ArgumentParser(description="寻一三倍量战法选股（诊断/回测）")
    parser.add_argument("--all", action="store_true", help="全 A 股 (默认仅自选股)")
    parser.add_argument("--days", type=int, default=None,
                        help="回看交易日 (默认: 当日模式 320 / 回测 550)")
    parser.add_argument("--top", type=int, default=20, help="当日模式每状态屏显条数 (默认 20)")
    parser.add_argument("--backtest", action="store_true", help="事件回测: 三类买点 → 规则出场收益")
    parser.add_argument("--self-test", action="store_true", help="运行内置自检后退出")
    parser.add_argument("--market-bull", action="store_true",
                        help="大盘空头时跳过筛选（默认仅标注，不拦截）")
    args = parser.parse_args()

    if args.self_test:
        self_test()
        return 0

    days = args.days or (BACKTEST_DAYS if args.backtest else LOOKBACK_DAYS)
    conn = connect()
    cursor = conn.cursor()
    names_by_code = load_stock_names(conn)

    # 大盘择时：标注 + 可选硬过滤（--market-bull）
    regime = market_regime(conn)
    print(f"[大盘] {market_line(regime)}", file=sys.stderr)
    if args.market_bull and regime and not regime["bull"]:
        sys.stderr.write("[大盘] 空头, --market-bull 下跳过\n")
        return 0

    t0 = time.time()
    today_str = time.strftime("%Y-%m-%d")
    db_codes = set(get_a_stock_codes(cursor))
    if args.all:
        pool = set(all_mainboard_codes(conn))
        pool_desc = f"全 A 股 {len(pool)} 只"
    else:
        pool = {parse_code(c) for c in zxg_codes()}
        pool_desc = f"自选股 zxg.blk {len(pool)} 只"
    codes = sorted(mc for mc in (db_codes & pool)
                   if (mc[0], mc[1][:2]) in {("sh", "60"), ("sz", "00"), ("sz", "30")}
                   and not is_excluded(mc[0], mc[1], names_by_code.get(f"{mc[0]}{mc[1]}", "")))
    if not codes:
        sys.stderr.write("无候选标的（加 --all 筛全市场）\n")
        return 1
    adj_by_mc = batch_fetch_adjust(conn, codes)

    start_date = (datetime.now() - timedelta(days=days * 3 // 2 + 70)).strftime("%Y-%m-%d")
    print(f"[1/3] {pool_desc} ∩ DB − 科创/北交/ST = {len(codes)} 只, 回看 {days} 交易日",
          file=sys.stderr)

    frames_meta = []       # 回测需要全量驻留; 当日模式逐只处理
    results = []
    for code, df in iter_stock_frames(cursor, codes, adj_by_mc, start_date):
        market, num = code[:2], code[2:]
        name = names_by_code.get(code, "")
        df = add_expma(df)
        if args.backtest:
            frames_meta.append((code, market, num, name, df))
            continue
        anchors = find_anchors(df)
        if not len(anchors):
            continue
        a = int(anchors[-1])                       # 最近三倍量锚
        st, red, age = classify_state(df, a, len(df) - 1)
        if st in ("破位", "观望"):
            continue
        t = df.iloc[-1]
        results.append({
            "代码": code, "名称": name, "现价": round(float(t["C"]), 2),
            "状态": st, "三倍量日": str(df["ts"].iloc[a])[:10],
            "量倍数": round(float(df["V"].iloc[a])
                           / max(float(df["V"].shift(1).rolling(5).mean().iloc[a]), 1e-9), 1),
            "红线": round(red, 2), "距红线%": round((float(t["C"]) / red - 1) * 100),
            "EXPMA5": round(float(t["E5"]), 2), "EXPMA12": round(float(t["E12"]), 2),
            "量低价": round(float(df["L"].iloc[a]), 2),
            "250日位": round(float(t["pos250"]), 2) if np.isfinite(t["pos250"]) else "",
            "锚龄": age,
        })
    conn.close()

    if args.backtest:
        d_min = min(df["ts"].iloc[0] for _, _, _, _, df in frames_meta) if frames_meta else ""
        d_max = max(df["ts"].iloc[-1] for _, _, _, _, df in frames_meta) if frames_meta else ""
        ev_df = run_backtest(frames_meta, f"{pool_desc} · {str(d_min)[:7]}~{str(d_max)[:7]}")
        if ev_df is not None:
            out_dir = os.path.join(OUTPUT_DIR, "find-trivol")
            os.makedirs(out_dir, exist_ok=True)
            stamped = os.path.join(out_dir, f"backtest-events-{time.strftime('%Y%m%d')}.csv")
            ev_df.to_csv(stamped, index=False)
            print(f"[csv] → {stamped} (共 {len(ev_df)} 事件)", file=sys.stderr)
        return 0

    if not results:
        print("无三倍量候选（最近 60 交易日内有三倍量且未破位的标的为空）")
        return 0
    df_out = pd.DataFrame(results)
    df_out["_prio"] = df_out["状态"].map({s: i for i, s in enumerate(STATE_ORDER)})
    df_out = df_out.sort_values(["_prio", "距红线%"]).drop(columns="_prio")

    print(f"\n{'='*110}")
    print(f" 寻一三倍量候选 [{pool_desc}] 数据截至 {today_str}（前复权; 前置: fetch-today 已入库当日 bar）")
    print(f"{'='*110}")
    for st in STATE_ORDER:
        g = df_out[df_out["状态"] == st].head(args.top)
        if g.empty:
            continue
        print(f"\n--- {st} (最多 {args.top} 只, 按距红线升序) ---")
        cols = ["代码", "名称", "现价", "红线", "距红线%", "EXPMA5", "EXPMA12", "量低价",
                "三倍量日", "量倍数", "锚龄", "250日位"]
        print("  ".join(cols))
        for _, x in g.iterrows():
            print("  ".join(str(x[cl]) for cl in cols))
    print(f"{'='*110}")
    print("买点: ①金叉=EXPMA12上穿红线未大涨 ②回调低吸=缩量回踩两线区间/红线不破量低 "
          "③突破=收盘上穿红线\n止损: 跌破红线或三倍量最低价; 持有不破 EXPMA12; 涨停/冲高卖出\n"
          "博主规则未经本人回测验证，仅供诊断; 尾盘执行", file=sys.stderr)
    out_dir = os.path.join(OUTPUT_DIR, "find-trivol")
    os.makedirs(out_dir, exist_ok=True)
    stamped = os.path.join(out_dir, f"find-trivol-{time.strftime('%Y%m%d')}.xlsx")
    df_out.to_excel(stamped, index=False)
    print(f"[xlsx] → {stamped} (共 {len(df_out)} 条, 耗时 {time.time()-t0:.0f}s)", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
