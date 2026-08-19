#!/usr/bin/env python3
"""find-macd-weekdc (周线 MACD 二次穿越) — 周线 DIFF 两次上穿 DEA，中间夹死叉。

结构: g1(首次建仓) → dc(死叉洗盘) → g2(二次穿越)
确认: g2 在零轴附近/上方 + 放量 + 站上周均线

用法:
  python3 scripts/find-macd-weekdc.py --zxg
  python3 scripts/find-macd-weekdc.py --all
  python3 scripts/find-macd-weekdc.py --dc-fresh 6 --dc-vol-ratio 1.5
"""
import argparse
import os
import sys
import time
from concurrent.futures import ThreadPoolExecutor, as_completed

import pandas as pd

from common import (OUTPUT_DIR, parse_code, zxg_codes, batch_fetch_adjust, pad,
                    forward_ret, market_line, market_regime, report_events)
from common import (connect, batch_fetch_klines, load_stock_names,
                                  _compute, to_weekly, SECTOR_INDICES,
                                  get_sector_for_code)


def detect_double_cross(w, fresh=4, lookback=30, zero_band=0.02,
                        vol_ratio=1.3, min_gap=3):
    """周线 MACD 二次穿越信号。"""
    if w is None or len(w) < 35:
        return None
    c = w["C"].astype(float)
    v = w["V"].astype(float)
    diff = c.ewm(span=12, adjust=False).mean() - c.ewm(span=26, adjust=False).mean()
    dea = diff.ewm(span=9, adjust=False).mean()
    dv, ev, n = diff.values, dea.values, len(w)

    gc, dc_at = [], []
    for i in range(1, n):
        if dv[i - 1] <= ev[i - 1] and dv[i] > ev[i]:
            gc.append(i)
        elif dv[i - 1] >= ev[i - 1] and dv[i] < ev[i]:
            dc_at.append(i)
    gc_win = [i for i in gc if i >= n - lookback]
    if len(gc_win) < 2:
        return None
    g2, g1 = gc_win[-1], gc_win[-2]
    if g2 - g1 < min_gap:
        return None
    if dc_at and dc_at[-1] > g2:
        return None
    if g2 < n - fresh:
        return None

    dea_g2 = float(dea.iloc[g2])
    price_g2 = float(c.iloc[g2])
    if dea_g2 < -zero_band * price_g2:
        return None

    last3 = v.iloc[-3:]
    vol_rising = len(last3) >= 3 and bool(last3.is_monotonic_increasing)
    wash_v = v.iloc[g1:g2].mean()
    v_now = float(v.iloc[-1])
    vol_spike = bool(wash_v and wash_v > 0 and v_now > vol_ratio * wash_v)

    ma_ok = sum(1 for p in (5, 10, 20, 60)
                if len(c) >= p and float(c.iloc[-1]) > float(c.rolling(p).mean().iloc[-1]))

    score = 50  # 结构分
    if vol_rising or vol_spike:
        score += 10
    if vol_rising and vol_spike:
        score += 5
    score += ma_ok * 5

    return {
        "dc_score": score,
        "dc_diff": round(float(dv[-1]), 3),
        "dc_dea": round(float(ev[-1]), 3),
        "dc_g2": str(w["ts"].iloc[g2])[:10],
        "dc_age": n - 1 - g2,
        "dc_zero": "上" if dea_g2 > 0 else "近",
        "dc_vol": ("增" if vol_rising else "") + ("放" if vol_spike else "") or "—",
        "dc_ma": f"MA{ma_ok}/4",
    }


def sliding_events(code, df, fresh, lookback, zero_band, vol_ratio, dc_min_score):
    """回测: 逐周截断周线重跑 detect_double_cross, 事件 = 二次金叉周 (g2, 按日期去重)。

    前瞻收益从 g2 周最后一个交易日（日线）起算，与其余脚本同口径 fwd=(5,20,65)。
    """
    weekly = to_weekly(df)
    closes = [float(x) for x in df["C"]]
    lows = [float(x) for x in df["L"]]
    dts = [str(x)[:10] for x in df["ts"]]
    evs = []
    seen = set()
    n = len(weekly)
    for k in range(35, n):
        sig = detect_double_cross(weekly.iloc[:k + 1], fresh, lookback, zero_band,
                                  vol_ratio)
        if not sig or sig["dc_score"] < dc_min_score:
            continue
        g2 = sig["dc_g2"]
        if g2 in seen:
            continue
        seen.add(g2)
        # 事件日 = g2 周最后一个交易日（g2 为周标签，取 ≤ g2 的最后交易日）
        j = next((di for di in range(len(dts) - 1, -1, -1) if dts[di] <= g2), None)
        if j is None:
            continue
        evs.append({"code": code, "date": g2, **forward_ret(closes, lows, j)})
    return evs


def main():
    ap = argparse.ArgumentParser(description="find-macd-weekdc 周线 MACD 二次穿越")
    ap.add_argument("--codes", nargs="*")
    ap.add_argument("--zxg", action="store_true")
    ap.add_argument("--all", action="store_true")
    ap.add_argument("--top", type=int, default=30)
    ap.add_argument("--dc-fresh", type=int, default=4)
    ap.add_argument("--dc-lookback", type=int, default=30)
    ap.add_argument("--dc-zero-band", type=float, default=0.02)
    ap.add_argument("--dc-vol-ratio", type=float, default=1.3)
    ap.add_argument("--dc-min-score", type=float, default=58)
    ap.add_argument("--limit", type=int)
    ap.add_argument("--jobs", type=int, default=8)
    ap.add_argument("--output-dir", default=os.path.join(OUTPUT_DIR, "find-macd-weekdc"))
    ap.add_argument("--market-bull", action="store_true",
                    help="大盘空头时跳过筛选（默认仅标注，不拦截）")
    ap.add_argument("--backtest", action="store_true",
                    help="滑窗回测: 周线信号→前瞻收益(+5/20/65)汇总")
    args = ap.parse_args()

    conn = connect()
    # 大盘择时：标注 + 可选硬过滤（--market-bull）
    regime = market_regime(conn)
    print(f"[大盘] {market_line(regime)}", file=sys.stderr)
    if args.market_bull and regime and not regime["bull"]:
        sys.stderr.write("[大盘] 空头, --market-bull 下跳过\n")
        return 0
    if args.codes:
        pool = [parse_code(c) for c in args.codes]
    elif args.all:
        from common import all_mainboard_codes
        pool = all_mainboard_codes(conn)
    else:
        pool = [parse_code(c) for c in zxg_codes()]
    if not pool:
        sys.exit("[error] empty pool")
    if args.limit:
        pool = pool[:args.limit]

    print(f"[pool] {len(pool)} stocks")
    names = load_stock_names(conn)

    print("[fetch] 批量拉取日线...")
    klines = batch_fetch_klines(conn, pool, days=800 if args.backtest else 400,
                                min_rows=180)
    adj_by_mc = batch_fetch_adjust(conn, pool)

    all_features = []
    with ThreadPoolExecutor(max_workers=args.jobs) as ex:
        futs = [ex.submit(_compute, m, c, klines[(m, c)], adj_by_mc.get((m, c)), False)
                for m, c in pool if (m, c) in klines]
        for f in as_completed(futs):
            try:
                r = f.result()
                if r:
                    all_features.append(r)
            except Exception as e:
                sys.stderr.write(f"[warn] {e}\n")
    print(f"[fetch] {len(all_features)} 只完成预处理")

    if args.backtest:
        bt_events = []
        for m, c, df in all_features:
            bt_events.extend(sliding_events(
                f"{m}{c}", df, args.dc_fresh, args.dc_lookback,
                args.dc_zero_band, args.dc_vol_ratio, args.dc_min_score))
        print(f"[backtest] 共 {len(bt_events)} 个事件", file=sys.stderr)
        report_events(bt_events, title="find-macd-weekdc 回测")
        return 0

    print("[dc] 检测周线 MACD 二次穿越...")
    dc_map = {}

    def _dc_task(df):
        return detect_double_cross(to_weekly(df), args.dc_fresh, args.dc_lookback,
                                   args.dc_zero_band, args.dc_vol_ratio)

    with ThreadPoolExecutor(max_workers=args.jobs) as ex:
        futs = {ex.submit(_dc_task, df): (m, c) for m, c, df in all_features}
        for f in as_completed(futs):
            mc = futs[f]
            try:
                sig = f.result()
                if sig:
                    dc_map[mc] = sig
            except Exception as e:
                sys.stderr.write(f"[warn] dc {mc}: {e}\n")
    print(f"[dc] {len(dc_map)} 只触发二次穿越")

    results = []
    for (m, c), sig in dc_map.items():
        if sig["dc_score"] < args.dc_min_score:
            continue
        row = {"market": m, "code": c, "name": names.get(f"{m}{c}", ""),
               "sector": SECTOR_INDICES.get(get_sector_for_code(c), ""), **sig}
        results.append(row)
    results.sort(key=lambda x: -x["dc_score"])

    os.makedirs(args.output_dir, exist_ok=True)
    if results:
        pd.DataFrame(results).round(3).to_csv(
            os.path.join(args.output_dir, f"find-macd-weekdc-{time.strftime('%Y%m%d')}.csv"),
            index=False)

    print(f"\n=== 周线MACD二次穿越 ({len(results)} 只, score>={args.dc_min_score}) ===")
    if results:
        print(f"{'代码':<11}{'名称':<16}{'DC分':>5} {'DIFF':>7} {'DEA':>7} "
              f"{'二次金叉':>10} {'龄':>3} {'零轴':<5}{'量':<6}{'周均线':<6}")
        for r in results[:args.top]:
            print(f"{r['market']}{r['code']:<8}{pad(r['name'], 16)}"
                  f"{r['dc_score']:5.1f} {r['dc_diff']:7.3f} {r['dc_dea']:7.3f} "
                  f"{r['dc_g2']:>10} {r['dc_age']:3d} {r['dc_zero']:<5}{r['dc_vol']:<6}{r['dc_ma']:<6}")


if __name__ == "__main__":
    main()
