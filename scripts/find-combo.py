#!/usr/bin/env python3
"""
================================================================================
 综合选股脚本 find-combo — terrain + trivol + weekdc + reversal + finish-eating
================================================================================

复用 5 个策略的检测逻辑, 对同一标的池逐股跑全部策略, 输出共识报告。
共振越多的标的 = 越高的多策略确认度。

策略角色（按寻底链条顺序）:
  terrain     下降谷底收敛   观察池生成（最早）
  reversal    底部反转确认   K线形态确认
  finish-eating 吸筹结束     筹码/户数确认（唯一用基本面数据）
  trivol      三倍量回调低吸  执行买点
  weekdc      周线二次金叉    趋势滤网（最确认）

用法:
  python3 scripts/find-combo.py                    # 默认自选股 zxg.blk
  python3 scripts/find-combo.py --all              # 全 A 股
  python3 scripts/find-combo.py --codes sh600985 sz002043
  python3 scripts/find-combo.py --min-hit 3        # 仅显示 ≥3 策略共振
  python3 scripts/find-combo.py --market-bull      # 大盘空头时跳过
================================================================================
"""

import importlib.util
import os
import sys
import time
import warnings
from collections import defaultdict
from datetime import datetime, timedelta

import numpy as np
import pandas as pd

sys.path.insert(0, os.path.dirname(__file__))
import common as C

# 加载 5 个策略模块
def _load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    m = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(m)
    return m

terrain = _load("terrain", "find-terrain.py")
trivol = _load("trivol", "find-trivol.py")
weekdc = _load("weekdc", "find-macd-weekdc.py")
reversal = _load("reversal", "find-reversal.py")
finish = _load("finish", "find-finish-eating.py")

DIR = os.path.dirname(__file__)


# ======================== 单股检测 ========================
def detect_terrain(df):
    """terrain: 检测下降谷底收敛 (class 5) run 首日。返回 bool。"""
    if df is None or len(df) < 120:
        return False
    with warnings.catch_warnings():
        warnings.simplefilter("ignore", RuntimeWarning)
        feats = terrain.terrain_series(df["C"], df["H"], df["L"])
    if feats is None:
        return False
    n = len(feats["elev"])
    prev = None
    for i in range(n):
        if np.isnan(feats["elev"][i]):
            continue
        cls = terrain.classify_arr(feats["elev"][i], feats["slope_t"][i],
                                   feats["elev_5"][i], feats["coh"][i])
        if cls == 5 and prev != 5:
            return True
        prev = cls
    return False


def detect_trivol(df):
    """trivol: 检测是否有有效三倍量锚 + 任一买点（金叉/回调低吸/突破）。"""
    if df is None or len(df) < 70:
        return False, ""
    try:
        anchors = trivol.find_anchors(df)
        if not anchors:
            return False, ""
        types = set()
        for a in anchors:
            for e in trivol.entries_for_anchor(df, a):
                types.add(e["type"])
        if not types:
            return False, ""
        label = "".join(
            ["金" if 0 in types else "", "低" if 1 in types else "", "突" if 2 in types else ""]
        )
        return True, label
    except Exception:
        return False, ""


def detect_weekdc(df):
    """weekdc: 周线 MACD 二次穿越（最近 4 周内）。"""
    if df is None or len(df) < 180:
        return False, 0
    try:
        weekly = C.to_weekly(df)
        sig = weekdc.detect_double_cross(weekly)
        if sig and sig["dc_score"] >= 58:
            return True, int(sig["dc_score"])
    except Exception:
        pass
    return False, 0


def detect_reversal(rows, float_shares):
    """reversal: 底部反转（60日回撤+锤头+假阴+高换手+二次探底+突破）。"""
    if not rows or len(rows) < 20:
        return False
    try:
        cfg = {"min_drawdown": 0.40, "min_amplitude": 0.08, "min_turnover": 15.0,
               "req_fake_bearish": True, "req_secondary_test": True, "req_breakout": True}
        ok, _ = reversal.screen_stock("", rows, float_shares, cfg)
        return ok
    except Exception:
        return False


def detect_finish(rows, shares, index_closes):
    """finish-eating: 吸筹结束突破（平台+地量+缩换手+放量突破）。"""
    if not rows:
        return False
    try:
        cfg = dict(plat_days=60, plat_amp=0.20, near_low=0.35, quiet_days=5,
                   vol_shrink=0.5, max_turnover=3.0, vol_ratio=2.0,
                   min_break=0.03, conc90=0.40)
        ok, _ = finish.screen_stock(rows, shares, index_closes, cfg)
        return ok
    except Exception:
        return False


# ======================== 主函数 ========================
def main():
    import argparse
    parser = argparse.ArgumentParser(description="综合选股 — 5 策略共识")
    parser.add_argument("--all", action="store_true", help="全 A 股 (默认自选股 zxg.blk)")
    parser.add_argument("--codes", nargs="*", help="指定代码 (如 sh600985 sz002043)")
    parser.add_argument("--min-hit", type=int, default=1,
                        help="仅显示不少于 N 策略共振 (默认 1 = 显示全部)")
    parser.add_argument("--market-bull", action="store_true",
                        help="大盘空头时跳过筛选（默认仅标注，不拦截）")
    parser.add_argument("--json", action="store_true", help="JSON 输出 (stdout)")
    args = parser.parse_args()

    conn = C.connect()
    cursor = conn.cursor()
    names_by_code = C.load_stock_names(conn)

    # 大盘择时
    regime = C.market_regime(conn)
    print(f"[大盘] {C.market_line(regime)}", file=sys.stderr)
    if args.market_bull and regime and not regime["bull"]:
        sys.stderr.write("[大盘] 空头, --market-bull 下跳过\n")
        return 0

    # 标的池
    if args.codes:
        pool = [C.parse_code(c) for c in args.codes]
    elif args.all:
        pool = list(C.all_mainboard_codes(conn))
        pool_desc = f"全 A 股 {len(pool)} 只"
    else:
        pool = [C.parse_code(c) for c in C.zxg_codes()]
        pool_desc = f"自选股 zxg.blk {len(pool)} 只"

    # DB 有数据的代码
    cursor.execute(r'SELECT table_name FROM information_schema.ins_tables WHERE table_name LIKE "k\_%\_1d"')
    db_codes = set()
    for t in cursor.fetchall():
        name = t[0]
        inner = name[2:-3]
        market, num = inner[:2], inner[2:]
        if len(num) == 6 and num.isdigit():
            if (market == "sh" and num[0] == "6") or \
               (market == "sz" and num[0] in "03") or \
               (market == "bj" and num[0] in "48"):
                db_codes.add((market, num))

    pool = sorted(set(pool) & db_codes)
    n_st = sum(1 for mc in pool if C.is_st_name(names_by_code.get(f"{mc[0]}{mc[1]}", "")))
    pool = [mc for mc in pool if not C.is_st_name(names_by_code.get(f"{mc[0]}{mc[1]}", ""))]
    print(f"[pool] {pool_desc} ∩ DB = {len(pool)} 只(排除 ST/*ST {n_st})",
          file=sys.stderr)
    if not pool:
        sys.stderr.write("无候选标的\n")
        return 1

    adj_by_mc = C.batch_fetch_adjust(conn, pool)
    t0 = time.time()

    # finish-eating 需要指数收盘（抗跌列，仅信息展示，不影响判定）
    idx_start = (datetime.now() - timedelta(days=600)).strftime("%Y-%m-%d")
    try:
        idx_rows = list(cursor.execute(
            f"SELECT ts, close FROM k_sh000001_1d WHERE ts >= '{idx_start}'"))
        index_closes = {str(r[0])[:10]: float(r[1]) for r in idx_rows}
    except Exception:
        index_closes = {}

    # reversal 需要流通股本
    shares_map = {}
    for mc in pool:
        try:
            cursor.execute(f"SELECT liutongguben FROM fn_{mc[1]}")
            r = cursor.fetchall()
            shares_map[mc] = float(r[0][0]) if r else None
        except Exception:
            shares_map[mc] = None

    # 逐股检测
    results = []  # (code, name, terrain, trivol, weekdc, reversal, finish, hits, detail)
    for mc in pool:
        code = f"{mc[0]}{mc[1]}"
        name = names_by_code.get(code, "")
        df = C.fetch_kline(conn, mc[0], mc[1], "1d", 900, 120)
        if df is None:
            continue
        df = df.copy()
        df["ts"] = pd.to_datetime(df["ts"])
        df = C.apply_qfq(df, adj_by_mc.get(mc))
        if df is None:
            continue

        # terrain
        hit_terrain = detect_terrain(df)
        # trivol
        hit_trivol, trivol_label = detect_trivol(df)
        # weekdc
        hit_weekdc, weekdc_score = detect_weekdc(df)
        # reversal (用行元组)
        rows = list(df.itertuples(index=False, name=None))
        # reversal 期望 (ts,o,h,l,c,v,code) — 补 code 列
        rows_code = [(r[0], r[1], r[2], r[3], r[4], r[5], code) for r in rows]
        hit_reversal = detect_reversal(rows_code, shares_map.get(mc))
        # finish-eating
        hit_finish = detect_finish(rows_code, shares_map.get(mc), index_closes)

        hits = sum([hit_terrain, hit_trivol, hit_weekdc, hit_reversal, hit_finish])
        if hits >= args.min_hit:
            detail = ""
            if hit_trivol:
                detail += f"[{trivol_label}]"
            if hit_weekdc:
                detail += f"[DC分{weekdc_score}]"
            results.append((code, name, hit_terrain, hit_trivol, hit_weekdc,
                            hit_reversal, hit_finish, hits, detail))

    conn.close()
    t1 = time.time()

    # 排序：共振数降序 → 代码升序
    results.sort(key=lambda x: (-x[7], x[0]))

    # 输出
    print(f"\n[combo] 检测 {len(pool)} 只, {len(results)} 只 ≥{args.min_hit} 策略共振"
          f" (耗时 {t1 - t0:.1f}s)\n", file=sys.stderr)

    if args.json:
        out = []
        for code, name, tr, tv, wd, rv, fe, hits, det in results:
            out.append({"code": code, "name": name, "hits": hits,
                        "terrain": tr, "trivol": tv, "weekdc": wd,
                        "reversal": rv, "finish_eating": fe, "detail": det})
        import json
        print(json.dumps(out, ensure_ascii=False, indent=2, default=str))
    else:
        print("=" * 96)
        print(f" 综合选股 — 5 策略共识 [{pool_desc}]")
        print(f" 策略: terrain谷底收敛 | reversal底部反转 | finish吸筹结束 | "
              f"trivol三倍量低吸 | weekdc周线金叉")
        print(f" 共 {len(pool)} 只, {len(results)} 只 ≥{args.min_hit} 策略共振")
        print("=" * 96)
        print(f"{'代码':<10} {'名称':<8} {'共振':>4} {'谷底':>4} {'反转':>4} "
              f"{'吸筹':>4} {'低吸':>4} {'周叉':>4} {'详情':<12}")
        print("-" * 96)
        for code, name, tr, tv, wd, rv, fe, hits, det in results[:50]:
            print(f"{code:<10} {name:<8} {hits:>4} "
                  f"{'✓' if tr else '·':>4} {'✓' if rv else '·':>4} "
                  f"{'✓' if fe else '·':>4} {'✓' if tv else '·':>4} "
                  f"{'✓' if wd else '·':>4} {det:<12}")
        print("=" * 96)
        if len(results) > 50:
            print(f"  (前 50 条, 共 {len(results)} 条)", file=sys.stderr)

    # 汇总统计
    counts = defaultdict(int)
    for _, _, tr, tv, wd, rv, fe, hits, _ in results:
        counts[hits] += 1
    print(f"\n[统计] 共振分布:", file=sys.stderr)
    for k in sorted(counts.keys(), reverse=True):
        print(f"       {k} 策略共振: {counts[k]} 只", file=sys.stderr)

    return 0


if __name__ == "__main__":
    sys.exit(main())
