#!/usr/bin/env python3
"""find-macd-mingc (分钟 MACD 信号) — 30m 底背离 + 5m 零轴金叉。

30m 底背离: 价 LL（新低）+ DIFF HL（抬高）→ 下跌动能衰竭
5m 金叉: DIFF 上穿 DEA, 交叉处 DEA>0 → 零轴上方确认

用法:
  python3 scripts/find-macd-mingc.py --zxg
  python3 scripts/find-macd-mingc.py --all
  python3 scripts/find-macd-mingc.py --div-fresh 5 --gc-fresh 48
"""
import argparse
import os
import sys
import time
from concurrent.futures import ThreadPoolExecutor, as_completed

import pandas as pd

from common import (OUTPUT_DIR, parse_code, zxg_codes, batch_fetch_adjust, apply_qfq, pad,
                    market_line, market_regime)
from common import (connect, fetch_kline, thread_conn, load_stock_names,
                                  resample_intraday, _pivot_lows,
                                  SECTOR_INDICES, get_sector_for_code)


def detect_bottom_div(bars, k=3, sep=4, fresh=8):
    """30m MACD 底背离: 最近两个波谷 p1<p2, 价新低但 DIFF 抬高。"""
    if bars is None or len(bars) < 35:
        return None
    c = bars["C"].astype(float)
    diff = c.ewm(span=12, adjust=False).mean() - c.ewm(span=26, adjust=False).mean()
    cv, dv, n = c.values, diff.values, len(c)
    piv = _pivot_lows(cv, k, sep)
    if len(piv) < 2:
        return None
    p1, p2 = piv[-2], piv[-1]
    if not (cv[p2] < cv[p1] and dv[p2] > dv[p1]):
        return None
    if p2 < n - fresh:
        return None
    return {
        "p2_ts": str(bars["ts"].iloc[p2])[:16],
        "age": n - 1 - p2,
        "price_drop": round((cv[p2] / cv[p1] - 1) * 100, 2),
        "diff_lift": round(float(dv[p2] - dv[p1]), 3),
        "rebound": bool(cv[-1] > cv[p2] and dv[-1] > dv[p2]),
    }


def detect_5m_gc(df5, fresh=24):
    """5m MACD 零轴上金叉。"""
    if df5 is None or len(df5) < 35:
        return None
    c = df5["C"].astype(float)
    diff = c.ewm(span=12, adjust=False).mean() - c.ewm(span=26, adjust=False).mean()
    dea = diff.ewm(span=9, adjust=False).mean()
    dv, ev, n = diff.values, dea.values, len(diff)
    gc = None
    for i in range(1, n):
        if dv[i - 1] <= ev[i - 1] and dv[i] > ev[i]:
            gc = i
    if gc is None or gc < n - fresh:
        return None
    if ev[gc] <= 0:
        return None
    return {
        "gc_ts": str(df5["ts"].iloc[gc])[:16],
        "gc_age": n - 1 - gc,
        "gc_diff": round(float(dv[gc]), 3),
        "gc_dea": round(float(ev[gc]), 3),
        "gc_rising": bool(dv[-1] > dv[gc]),
    }


def detect_div(df5, pivot_k=3, fresh_days=3, gc_fresh=24):
    """30m 底背离 + 5m 零轴金叉 组合检测。"""
    bars30 = resample_intraday(df5, "30min")
    div30 = detect_bottom_div(bars30, k=pivot_k, fresh=fresh_days * 8)
    gc5 = detect_5m_gc(df5, fresh=gc_fresh)

    if not div30 and not gc5:
        return None

    score, tf = 0, ""
    if div30:
        score += 45
        tf = "30m背离"
    if gc5:
        score += 30
        if gc5.get("gc_rising"):
            score += 10
        tf = tf + "+5m金叉" if tf else "5m金叉"
    if div30 and gc5:
        score += 15  # 共振加分
        tf = "共振(背离+金叉)"

    return {
        "div_score": score, "div_tf": tf,
        **{f"div_{k}": v for k, v in (div30 or {}).items()},
        **{k: v for k, v in (gc5 or {}).items()},
    }


def main():
    ap = argparse.ArgumentParser(description="find-macd-mingc 30m底背离+5m零轴金叉")
    ap.add_argument("--codes", nargs="*")
    ap.add_argument("--zxg", action="store_true")
    ap.add_argument("--all", action="store_true")
    ap.add_argument("--top", type=int, default=30)
    ap.add_argument("--div-days", type=int, default=30, help="取近 N 日 5m 线")
    ap.add_argument("--div-pivot", type=int, default=3)
    ap.add_argument("--div-fresh", type=int, default=3)
    ap.add_argument("--gc-fresh", type=int, default=24)
    ap.add_argument("--div-min-score", type=float, default=40)
    ap.add_argument("--limit", type=int)
    ap.add_argument("--jobs", type=int, default=8)
    ap.add_argument("--output-dir", default=os.path.join(OUTPUT_DIR, "find-macd-mingc"))
    ap.add_argument("--market-bull", action="store_true",
                    help="大盘空头时跳过筛选（默认仅标注，不拦截）")
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
    adj_by_mc = batch_fetch_adjust(conn, pool)

    print("[div] 检测 30m MACD 底背离 + 5m 零轴金叉...")
    div_map = {}

    def _div_task(m, c):
        df5 = fetch_kline(thread_conn(), m, c, cycle="5m", days=args.div_days, min_rows=200)
        if df5 is None:
            return None
        return detect_div(apply_qfq(df5, adj_by_mc.get((m, c))),
                          args.div_pivot, args.div_fresh, args.gc_fresh)

    with ThreadPoolExecutor(max_workers=args.jobs) as ex:
        futs = {ex.submit(_div_task, m, c): (m, c) for m, c in pool}
        for f in as_completed(futs):
            mc = futs[f]
            try:
                sig = f.result()
                if sig:
                    div_map[mc] = sig
            except Exception as e:
                sys.stderr.write(f"[warn] div {mc}: {e}\n")
    print(f"[div] {len(div_map)} 只触发")

    results = []
    for (m, c), sig in div_map.items():
        if sig["div_score"] < args.div_min_score:
            continue
        row = {"market": m, "code": c, "name": names.get(f"{m}{c}", ""),
               "sector": SECTOR_INDICES.get(get_sector_for_code(c), ""), **sig}
        results.append(row)
    results.sort(key=lambda x: -x["div_score"])

    os.makedirs(args.output_dir, exist_ok=True)
    if results:
        pd.DataFrame(results).round(3).to_csv(
            os.path.join(args.output_dir, f"find-macd-mingc-{time.strftime('%Y%m%d')}.csv"),
            index=False)

    # 底背离明细
    div_rows = [r for r in results if "背离" in r.get("div_tf", "")]
    if div_rows:
        print(f"\n=== 30m MACD底背离 ({len(div_rows)} 只) ===")
        print(f"{'代码':<11}{'名称':<16}{'DIV分':>5} {'背离低':>16} {'价跌%':>6} {'DIFF抬':>7} {'反弹':<4}")
        for r in div_rows[:args.top]:
            print(f"{r['market']}{r['code']:<8}{pad(r['name'], 16)}"
                  f"{r['div_score']:5.1f} {r.get('div_p2_ts', ''):>16} "
                  f"{r.get('div_price_drop', 0):6.2f} {r.get('div_diff_lift', 0):7.3f} "
                  f"{'是' if r.get('div_rebound') else '否'}")

    # 5m 金叉明细
    gc_rows = [r for r in results if "金叉" in r.get("div_tf", "")]
    if gc_rows:
        print(f"\n=== 5m MACD零轴金叉 ({len(gc_rows)} 只) ===")
        print(f"{'代码':<11}{'名称':<16}{'DIV分':>5} {'金叉时':>16} {'DIFF':>7} {'DEA':>7} {'龄':>3} {'上扬':<4}")
        for r in gc_rows[:args.top]:
            print(f"{r['market']}{r['code']:<8}{pad(r['name'], 16)}"
                  f"{r['div_score']:5.1f} {r.get('gc_ts', ''):>16} "
                  f"{r.get('gc_diff', 0):7.3f} {r.get('gc_dea', 0):7.3f} "
                  f"{r.get('gc_age', 0):3d} {'是' if r.get('gc_rising') else '否'}")


if __name__ == "__main__":
    main()
