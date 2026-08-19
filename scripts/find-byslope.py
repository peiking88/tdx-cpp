#!/usr/bin/env python3
"""
================================================================================
 均线斜率评估/策略脚本 — 金叉动能分级（归一化斜率版）
================================================================================

源自「均线加斜率 = 从二维到三维」的交易思路：
  同样的 MA5 上穿 MA20（金叉），5 日均线斜率陡峭（资金持续流入）的走主升，
  斜率平缓的（多空勉强平衡）大概率拐头失败。文章的「30/45 度」是图表几何，
  依赖纵轴缩放——本脚本用归一化斜率（均线变化率 %/日）替代，阈值由数据标定。

两种模式：
  评估（默认）: 回看全部金叉日，按金叉当日 MA5 斜率分桶，统计 +5/+10/+20 日
                前瞻收益与胜率 → 验证斜率分级是否有效、标定开仓阈值
  --today    : 输出最近 N 个交易日内发生金叉的标的（--cross-within），按当前
                MA5 斜率降序（列含 MA5/MA20 斜率、现价距 MA5/MA20，阈值自定）

斜率定义: slope(MA_n) = (MA_n[t] - MA_n[t-k]) / MA_n[t-k] / k × 100  (%/日, k=--k)
信号定义: MA5 上穿 MA20（前一日 MA5<=MA20，当日 MA5>MA20）
日线前复权。默认自选股，--all 全市场。

用法:
  python3 scripts/find-byslope.py                # 评估自选股近一年金叉分桶收益
  python3 scripts/find-byslope.py --all          # 全市场评估（标定阈值用这个）
  python3 scripts/find-byslope.py --all --today  # 当前金叉标的清单（写 xlsx）
================================================================================
"""

import argparse
import os
import sys
import time
from collections import defaultdict
from datetime import datetime, timedelta

import pandas as pd
import taosws

from common import (OUTPUT_DIR, all_mainboard_codes, apply_qfq,
                    batch_fetch_adjust, market_line, market_regime,
                    parse_code, zxg_codes)

# ======================== 配置 ========================
TDENGINE_HOST = os.environ.get("TDENGINE_HOST", "localhost")
TDENGINE_PORT = int(os.environ.get("TDENGINE_PORT", "6041"))
TDENGINE_USER = os.environ.get("TDENGINE_USER", "root")
TDENGINE_PASS = os.environ.get("TDENGINE_PASS", "taosdata")
TDENGINE_DB = os.environ.get("TDENGINE_DB", "tdx")

BATCH_SIZE = 1800
EVAL_DAYS = 250        # 评估回看交易日
SLOPE_K = 3            # 斜率窗口（日）
FWD_PERIODS = (5, 10, 20)   # 前瞻收益区间（交易日）
SLOPE_BUCKETS = (0, 1, 2, 4)  # 分桶边界 (%/日): <0 | 0-1 | 1-2 | 2-4 | >=4
CROSS_WITHIN = 3       # --today: 金叉发生在此交易日数内


# ======================== 数据获取（对齐 find-retrace 惯例） ========================
def get_a_stock_codes(cursor):
    cursor.execute(r'SELECT table_name FROM information_schema.ins_tables WHERE table_name LIKE "k\_%\_1d"')
    a_codes = []
    for t in cursor.fetchall():
        inner = t[0][2:-3]
        market, num = inner[:2], inner[2:]
        if len(num) == 6 and num.isdigit():
            if (market == "sh" and num[0] == "6") or \
               (market == "sz" and num[0] in "03") or \
               (market == "bj" and num[0] in "48"):
                a_codes.append((market, num))
    return a_codes


def batch_query_kline(cursor, codes, start_date):
    queries = []
    for market, num in codes:
        queries.append(
            f"SELECT ts, open, high, low, close, volume, '{market}{num}' as code "
            f"FROM k_{market}{num}_1d WHERE ts >= '{start_date}'"
        )
    cursor.execute(" UNION ALL ".join(queries) + " ORDER BY code, ts")
    return cursor.fetchall()


def load_stock_names(conn):
    try:
        return {f"{m}{c}": n for m, c, n in conn.query(
            "SELECT market, code, name FROM tdx.stock_name")}
    except Exception:
        return {}


def qfq_rows(rows, events):
    if not events:
        return rows
    df = pd.DataFrame(rows, columns=["ts", "O", "H", "L", "C", "V", "code"])
    df["ts"] = pd.to_datetime(df["ts"])
    df = apply_qfq(df, events)
    return list(df.itertuples(index=False, name=None))


# ======================== 斜率信号核心 ========================
def slope_series(ma, k):
    """均线 n 日变化率归一斜率（%/日）。ma: pd.Series。"""
    return (ma - ma.shift(k)) / ma.shift(k) / k * 100.0


def cross_signals(rows, k):
    """单只标的 → 信号 DataFrame。

    rows: (ts,O,H,L,C,V,code) 升序元组（前复权）。
    返回列: date, slope5, slope20, 以及各前瞻期收益 ret_5/ret_10/ret_20（不足为 NaN）。
    """
    df = pd.DataFrame([r[:5] for r in rows], columns=["ts", "O", "H", "L", "C"])
    if len(df) < 25 + k:
        return None
    ma5 = df["C"].rolling(5).mean()
    ma20 = df["C"].rolling(20).mean()
    s5 = slope_series(ma5, k)
    s20 = slope_series(ma20, k)
    cross = (ma5 > ma20) & (ma5.shift(1) <= ma20.shift(1)) & s5.notna()

    out = pd.DataFrame({
        "ts": df["ts"][cross],
        "ma5": ma5[cross], "ma20": ma20[cross],
        "slope5": s5[cross], "slope20": s20[cross],
        "close": df["C"][cross],
    })
    if out.empty:
        return None
    for p in FWD_PERIODS:
        out[f"ret_{p}"] = df["C"].shift(-p)[cross] / out["close"] - 1.0
    return out


def bucket_of(slope):
    if slope < SLOPE_BUCKETS[0]:
        return "<0"
    for i, edge in enumerate(SLOPE_BUCKETS):
        if slope < edge:
            return f"{SLOPE_BUCKETS[i-1]}-{edge}" if i else f"0-{edge}"
    return f">={SLOPE_BUCKETS[-1]}"


# ======================== 自检 ========================
def self_test():
    import numpy as np
    # 构造: 30 日横盘 10.0 → 线性拉升（金叉发生在拉升初期, MA5 斜率刚转正）
    closes = [10.0] * 30 + [10.0 + 0.3 * i for i in range(1, 11)]
    rows = [(datetime(2026, 7, 1) + timedelta(days=i), 10, 10, 10, c, 0, "sh600000")
            for i, c in enumerate(closes)]
    sig = cross_signals(rows, k=3)
    assert sig is not None and len(sig) == 1, sig
    r = sig.iloc[0]
    assert r["slope5"] > 0.1, r  # 金叉日 MA5 斜率刚转正（拉升初期）
    assert r["slope20"] < r["slope5"], r  # MA20 更迟钝
    # 前瞻收益: 信号日之后 5 日 close 已知
    t = int(r["ts"].strftime("%d")) - 1
    assert abs(r["ret_5"] - (closes[t + 5] / closes[t] - 1)) < 1e-9, r
    # 无金叉（一路下跌）→ 无信号
    down = [(datetime(2026, 7, 1) + timedelta(days=i), 10, 10, 10, 12.0 - 0.1 * i, 0, "sh600000")
            for i in range(60)]
    assert cross_signals(down, k=3) is None
    # 分桶
    assert bucket_of(-0.1) == "<0" and bucket_of(0.5) == "0-1" and \
        bucket_of(1.5) == "1-2" and bucket_of(3) == "2-4" and bucket_of(5) == ">=4"
    print("self-test OK")


# ======================== 主函数 ========================
def main():
    parser = argparse.ArgumentParser(description="均线斜率评估/策略 — 金叉动能分级")
    parser.add_argument("--all", action="store_true", help="全 A 股 (默认仅自选股 zxg.blk)")
    parser.add_argument("--days", type=int, default=EVAL_DAYS, help="评估回看交易日 (默认 250)")
    parser.add_argument("--k", type=int, default=SLOPE_K, help="斜率窗口日 (默认 3)")
    parser.add_argument("--today", action="store_true",
                        help="输出最近金叉标的清单（默认为分桶评估）")
    parser.add_argument("--cross-within", type=int, default=CROSS_WITHIN,
                        help="--today: 金叉发生在最近 N 个交易日内 (默认 3)")
    parser.add_argument("--self-test", action="store_true", help="运行内置自检后退出")
    parser.add_argument("--market-bull", action="store_true",
                        help="大盘空头时跳过筛选（默认仅标注，不拦截）")
    args = parser.parse_args()

    if args.self_test:
        self_test()
        return 0

    url = f"taosws://{TDENGINE_USER}:{TDENGINE_PASS}@{TDENGINE_HOST}:{TDENGINE_PORT}/{TDENGINE_DB}"
    conn = taosws.connect(url)
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
    a_stocks = sorted(db_codes & pool)
    if not a_stocks:
        sys.stderr.write("无候选标的（加 --all 筛全市场）\n")
        return 1
    adj_by_mc = batch_fetch_adjust(conn, a_stocks)

    # 多取 40 交易日余量（MA20 + 斜率窗口 + 前瞻期）
    start_date = (datetime.now() - timedelta(days=args.days * 3 // 2 + 70)).strftime("%Y-%m-%d")
    print(f"[1/3] {pool_desc} ∩ DB = {len(a_stocks)} 只 (耗时 {time.time()-t0:.1f}s)", file=sys.stderr)

    signals = []           # 评估用: (slope5, slope20, ret_5, ret_10, ret_20)
    latest = {}            # code → (现价, 距MA5%, 距MA20%, 当前斜率5)
    cross_recent = []      # --today 用: (code, 金叉日, 金叉日slope5, 金叉日slope20)
    total_batches = (len(a_stocks) + BATCH_SIZE - 1) // BATCH_SIZE

    for bi in range(total_batches):
        batch = a_stocks[bi * BATCH_SIZE:(bi + 1) * BATCH_SIZE]
        t2 = time.time()
        kline_rows = batch_query_kline(cursor, batch, start_date)
        stock_data = defaultdict(list)
        for row in kline_rows:
            stock_data[row[6]].append(row)

        for market, num in batch:
            code = f"{market}{num}"
            rows = qfq_rows(stock_data.get(code, []), adj_by_mc.get((market, num)))
            if len(rows) < 25:
                continue
            closes = pd.Series([float(r[4]) for r in rows])
            ma5 = closes.rolling(5).mean()
            ma20 = closes.rolling(20).mean()
            latest[code] = (round(closes.iloc[-1], 2),
                            round((closes.iloc[-1] / ma5.iloc[-1] - 1) * 100, 1),
                            round((closes.iloc[-1] / ma20.iloc[-1] - 1) * 100, 1),
                            round(slope_series(ma5, args.k).iloc[-1], 2))
            sig = cross_signals(rows, args.k)
            if sig is None:
                continue
            # 评估: 剔除今日未完成 bar，取回看窗口内信号
            sig_hist = sig[sig["ts"].dt.strftime("%Y-%m-%d") != today_str].iloc[-(args.days):]
            for _, r in sig_hist.iterrows():
                signals.append((r["slope5"], r["slope20"],
                                *[r[f"ret_{p}"] for p in FWD_PERIODS]))
            # --today: 最近 N 个交易日内发生的金叉（按日期窗，非最近 N 个信号）
            since = rows[-args.cross_within][0]
            for _, r in sig[sig["ts"] >= since].iterrows():
                cross_recent.append((code, r["ts"].strftime("%Y-%m-%d"),
                                     round(r["slope5"], 2), round(r["slope20"], 2)))
        print(f"[2/3] 批次 {bi+1}/{total_batches}: kline {time.time()-t2:.1f}s, "
              f"累计信号 {len(signals)}", file=sys.stderr)

    conn.close()

    if not args.today:
        # -------- 评估模式: 按 MA5 斜率分桶统计前瞻收益 --------
        if not signals:
            print("回看期内无金叉信号")
            return 0
        df = pd.DataFrame(signals, columns=["slope5", "slope20"] + [f"ret_{p}" for p in FWD_PERIODS])
        df["bucket"] = df["slope5"].map(bucket_of)
        order = ["<0"] + [f"{SLOPE_BUCKETS[i-1]}-{e}" if i else f"0-{e}"
                          for i, e in enumerate(SLOPE_BUCKETS)][1:] + [f">={SLOPE_BUCKETS[-1]}"]
        print(f"\n{'='*92}")
        print(f" 金叉动能分级评估 [{pool_desc}] 斜率窗口 k={args.k} (%/日), 回看 {args.days} 交易日")
        print(f" 样本 {len(df)} 个金叉日 | 前瞻收益按信号日收盘计")
        print(f"{'='*92}")
        print(f"{'MA5斜率%/日':<12} {'样本':>6} {'+5日均':>8} {'+10日均':>8} "
              f"{'+20日均':>8} {'+20日胜率':>8} {'MA20斜率均值':>10}")
        print("-" * 92)
        for b in order:
            g = df[df["bucket"] == b]
            if g.empty:
                continue
            line = (f"{b:<12} {len(g):>6}")
            for p in FWD_PERIODS:
                v = g[f"ret_{p}"].dropna()
                line += f" {v.mean():>7.1%}" if len(v) else "      --"
            v20 = g["ret_20"].dropna()
            line += f" {(v20 > 0).mean():>8.1%}" if len(v20) else "       --"
            line += f" {g['slope20'].mean():>9.2f}"
            print(line)
        all_v20 = df["ret_20"].dropna()
        print("-" * 92)
        print(f"{'全体金叉':<12} {len(df):>6}"
              + "".join(f" {df[f'ret_{p}'].dropna().mean():>7.1%}" for p in FWD_PERIODS)
              + f" {(all_v20 > 0).mean():>8.1%} {df['slope20'].mean():>9.2f}")
        print(f"{'='*92}")
        print("读法: 若 +20 日收益/胜率随斜率单调上升 → 文章思路成立，"
              "开仓阈值取收益开始显著抬升的桶下沿", file=sys.stderr)
        return 0

    # -------- --today 模式: 最近金叉清单，按当前 MA5 斜率降序 --------
    out_dir = os.path.join(OUTPUT_DIR, "find-byslope")
    os.makedirs(out_dir, exist_ok=True)
    xlsx_rows = []
    for code, cross_date, s5, s20 in cross_recent:
        cur, dma5, dma20, cur_s5 = latest.get(code, (None, None, None, None))
        if cur_s5 is None:
            continue
        xlsx_rows.append({
            "代码": code, "名称": names_by_code.get(code, ""),
            "金叉日": cross_date, "金叉日斜率5": s5, "金叉日斜率20": s20,
            "当前斜率5": cur_s5, "现价": cur, "距MA5%": dma5, "距MA20%": dma20,
        })
    df = pd.DataFrame(xlsx_rows).sort_values("当前斜率5", ascending=False)
    stamp = time.strftime("%Y%m%d")
    stamped = os.path.join(out_dir, f"find-byslope-{stamp}.xlsx")
    df.to_excel(stamped, index=False)

    print(f"\n{'='*100}")
    print(f" 最近 {args.cross_within} 交易日金叉标的 [{pool_desc}] 按当前 MA5 斜率降序 (k={args.k}, %/日)")
    print(f"{'='*100}")
    cols = ["代码", "名称", "金叉日", "金叉日斜率5", "当前斜率5", "金叉日斜率20",
            "现价", "距MA5%", "距MA20%"]
    print("  ".join(f"{c:<10}" for c in cols))
    for _, x in df.head(50).iterrows():
        print("  ".join(f"{str(x.get(c, '')):<10}" for c in cols))
    print(f"{'='*100}")
    print(f"[xlsx] → {stamped} (共 {len(df)} 条)", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
