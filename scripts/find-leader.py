#!/usr/bin/env python3
"""find-leader (龙头漏斗) — 四层技术指标漏斗选股。

硬过滤: ret_250>0, MA5>MA20, ADX>20, +DI>-DI, 无长上影
评分 (0-100): RPS动量(30) + 近新高(20) + 均线多头(15) + RSI(10) + CCI(10) + 口袋支点(15)

用法:
  python3 scripts/find-leader.py --zxg           # 自选股
  python3 scripts/find-leader.py --all           # 全市场
  python3 scripts/find-leader.py --min-score 60  # 提高入选阈值
"""
import argparse
import os
import sys
import time
from concurrent.futures import ThreadPoolExecutor, as_completed

import numpy as np
import pandas as pd

from common import OUTPUT_DIR, parse_code, zxg_codes, batch_fetch_adjust, pad
from common import (connect, batch_fetch_klines, load_stock_names,
                                  _compute, sector_momentum, get_sector_for_code,
                                  SECTOR_INDICES)


def hard_filter_fail(last, df):
    """六步硬过滤失败原因 (None=通过)。"""
    ret_250 = last.get("ret_250")
    if pd.isna(ret_250) or ret_250 <= 0:
        return "no_ret"
    if pd.isna(last["MA5"]) or pd.isna(last["MA20"]) or last["MA5"] <= last["MA20"]:
        return "MA5<MA20"
    if pd.isna(last["ADX"]) or last["ADX"] <= 20:
        return "ADX<20"
    if pd.isna(last["plus_DI"]) or pd.isna(last["minus_DI"]) or last["plus_DI"] <= last["minus_DI"]:
        return "+DI<-DI"
    if df.tail(5)["long_upper"].any():
        return "long_upper"
    return None


def score_stock(df, sector_ret):
    """对单只股票评分, 返回 dict 或 None。"""
    if df is None or len(df) < 252:
        return None
    last = df.iloc[-1]
    if hard_filter_fail(last, df):
        return None
    ret_250 = last["ret_250"]

    score = 0
    details = {}
    mom_score = min(ret_250 / 2, 30)
    score += mom_score
    details["momentum"] = round(mom_score, 1)

    near_high = last.get("near_high", 0)
    high_score = max(0, min((near_high - 0.7) / 0.2 * 20, 20))
    score += high_score
    details["high_score"] = round(high_score, 1)

    ma_score = 0
    if last["MA5"] > last["MA20"]:
        ma_score += 7
    if last["MA20"] > last.get("MA60", 0) and not pd.isna(last.get("MA60")):
        ma_score += 8
    score += ma_score
    details["ma_align"] = ma_score

    rsi = last["RSI14"] if not pd.isna(last["RSI14"]) else 50
    rsi_score = max(0, 10 - abs(rsi - 60) / 5)
    score += rsi_score
    details["rsi"] = round(rsi_score, 1)

    cci = last["CCI"] if not pd.isna(last["CCI"]) else 0
    cci_score = min(max(cci / 100, 0), 1) * 10
    score += cci_score
    details["cci"] = round(cci_score, 1)

    score += max(0, sector_ret) / 5
    details["sector_mom"] = round(max(0, sector_ret) / 5, 1)

    return {
        "score": round(score, 1), "price": round(last["C"], 2),
        "ret_250": round(ret_250, 1), "near_high": round(near_high, 3),
        "RSI14": round(rsi, 1), "ADX": round(last["ADX"], 1),
        "CCI": round(cci, 1), "ATR_pct": round(last.get("ATR_pct", 0) or 0, 2),
        **details,
    }


def main():
    ap = argparse.ArgumentParser(description="find-leader 龙头漏斗选股")
    ap.add_argument("--codes", nargs="*", help="指定代码")
    ap.add_argument("--zxg", action="store_true", help="自选股")
    ap.add_argument("--all", action="store_true", help="全市场")
    ap.add_argument("--top", type=int, default=20)
    ap.add_argument("--min-score", type=float, default=40, help="最低评分")
    ap.add_argument("--rps", type=float, default=80, help="RPS 最低分位")
    ap.add_argument("--diagnostic", action="store_true")
    ap.add_argument("--limit", type=int)
    ap.add_argument("--jobs", type=int, default=8)
    ap.add_argument("--output-dir", default=os.path.join(OUTPUT_DIR, "find-leader"))
    args = ap.parse_args()

    conn = connect()
    if args.codes:
        pool = [parse_code(c) for c in args.codes]
    elif args.all:
        from common import all_mainboard_codes
        pool = all_mainboard_codes(conn)
    else:
        pool = [parse_code(c) for c in zxg_codes()]
    if not pool:
        print("[error] empty pool", file=sys.stderr)
        sys.exit(1)
    if args.limit:
        pool = pool[:args.limit]

    print(f"[pool] {len(pool)} stocks")
    names = load_stock_names(conn)
    print("[sector] computing momentum...")
    sec_mom = sector_momentum(conn)

    print("[fetch] 批量拉取日线...")
    klines = batch_fetch_klines(conn, pool, days=400, min_rows=252)
    adj_by_mc = batch_fetch_adjust(conn, pool)

    all_features = []
    with ThreadPoolExecutor(max_workers=args.jobs) as ex:
        futs = [ex.submit(_compute, m, c, klines[(m, c)], adj_by_mc.get((m, c)), True)
                for m, c in pool if (m, c) in klines]
        for f in as_completed(futs):
            try:
                r = f.result()
                if r:
                    all_features.append(r)
            except Exception as e:
                sys.stderr.write(f"[warn] {e}\n")
    print(f"[fetch] {len(all_features)} 只完成预处理")

    all_ret = [(m, c, df.iloc[-1]["ret_250"]) for m, c, df in all_features
               if not pd.isna(df.iloc[-1].get("ret_250"))]
    rets = np.array([x[2] for x in all_ret]) if all_ret else np.array([])

    results, diagnostic = [], []
    for m, c, df in all_features:
        last_ret = df.iloc[-1]["ret_250"] if not pd.isna(df.iloc[-1].get("ret_250")) else None
        if last_ret is None:
            continue
        rps = (rets < last_ret).mean() * 100 if len(rets) else 0
        if rps < args.rps:
            continue
        sector = get_sector_for_code(c)
        sec_ret = sec_mom.get(sector, 0)
        result = score_stock(df, sec_ret)
        if result is None:
            if args.diagnostic:
                last = df.iloc[-1]
                diagnostic.append({"market": m, "code": c, "RPS": round(rps, 1),
                                   "price": round(last["C"], 2),
                                   "reason": hard_filter_fail(last, df) or "filtered"})
            continue
        result["market"] = m
        result["code"] = c
        result["name"] = names.get(f"{m}{c}", "")
        result["RPS"] = round(rps, 1)
        result["sector"] = SECTOR_INDICES.get(sector, sector)
        results.append(result)

    results.sort(key=lambda x: -x["score"])
    filtered = [r for r in results if r["score"] >= args.min_score]

    os.makedirs(args.output_dir, exist_ok=True)
    if results:
        pd.DataFrame(results).round(3).to_csv(
            os.path.join(args.output_dir, f"find-leader-{time.strftime('%Y%m%d')}.csv"),
            index=False)

    print(f"\n=== 龙头候选 (top {args.top}, score>={args.min_score}) ===")
    if not filtered:
        print("  (无通过过滤的股票)")
    else:
        for i, r in enumerate(filtered[:args.top], 1):
            print(f"{i:4d} {r['market']}{r['code']:<8} {pad(r.get('name',''), 12)} "
                  f"{r['score']:6.1f} {r['RPS']:6.1f} {r['price']:8.2f} "
                  f"{r['ret_250']:8.1f} {r.get('sector','')}")

    if diagnostic:
        print(f"\n--- 诊断: RPS>{args.rps} 但未通过硬过滤 ({len(diagnostic)} 只) ---")
        for d in diagnostic[:10]:
            print(f"  {d['market']}{d['code']} RPS={d['RPS']} 原因={d['reason']}")


if __name__ == "__main__":
    main()
