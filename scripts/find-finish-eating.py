#!/usr/bin/env python3
"""
================================================================================
 吸筹结束突破筛选脚本 find-finish-eating.py — 「券商吸筹结束的判断标准」量化版
================================================================================

源文章五大维度（控盘度/筹码结构/量价行为/市场情绪/放量突破）中，可从日线
量价数据量化的是三条主线，其余维度（股东户数、板块热度、主动性买盘占比）
无数据源，已用量价代理：

  平台（筹码结构代理）: 长期横盘、低位、振幅收敛 —— 单峰密集的形态学近似
  地量（量价行为）    : 突破前换手萎缩至平台峰值的地量水平（文章: 高峰的30-50%）
  突破（最终确认）    : 放量大阳（量≥前日2倍）收盘站稳平台上沿（颈线位）

筛选条件（日线前复权）:
  1. 平台: --plat-days(60) 交易日（截至突破前一日）—— 振幅为参考因子不淘汰，
           振幅 <= --plat-amp(20%) 标注「紧致★」
  2. 低位: 平台收盘中位数距一年最低 low <= --near-low(35%)   [低位单峰]
  3. 地量: 突破前 --quiet-days(5) 日均量 <= 平台峰值量 × --vol-shrink(0.5)
  4. 换手: 安静期日均换手 <= --max-turnover(3%)（无流通股本数据时跳过）
  5. 情绪: 平台期换手率线性斜率 <= 0（散户离场）；
           大盘(sh000001)下跌日个股平均超额收益作信息列（主力护盘代理）
  6. 筹码: 换手率衰减筹码分布自算 90% 成本集中度 <= --conc90(40%)，
           套牢盘(现价上方筹码占比)作信息列
  7. 户数: F10 股东研究「股东人数变化」最新期变动比率 < 0（持续减少；无 F10 数据跳过）
  8. 突破: 最新 bar 量 >= 前日 × --vol-ratio(2.0)，收盘 >= 平台上沿，
           涨幅 >= --min-break(3%)

用法:
  python3 scripts/find-finish-eating.py                # 默认自选股（建议尾盘运行, 量更真实）
  python3 scripts/find-finish-eating.py --all          # 全 A 股
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
                    batch_fetch_adjust, forward_ret, is_st_name,
                    market_line, market_regime, parse_code,
                    report_events, zxg_codes)

# ======================== 配置 ========================
TDENGINE_HOST = os.environ.get("TDENGINE_HOST", "localhost")
TDENGINE_PORT = int(os.environ.get("TDENGINE_PORT", "6041"))
TDENGINE_USER = os.environ.get("TDENGINE_USER", "root")
TDENGINE_PASS = os.environ.get("TDENGINE_PASS", "taosdata")
TDENGINE_DB = os.environ.get("TDENGINE_DB", "tdx")

BATCH_SIZE = 1800
SCAN_DAYS = 250  # 一年低点回看（交易日）

PLAT_DAYS = 60        # 平台窗口（交易日）
PLAT_AMP = 0.20       # 平台最大振幅
NEAR_LOW = 0.35       # 平台中位价距一年最低 low 上限
QUIET_DAYS = 5        # 地量窗口（突破前）
VOL_SHRINK = 0.5      # 地量/平台峰值量 上限
MAX_TURNOVER = 3.0    # 安静期日均换手 % 上限
VOL_RATIO = 2.0       # 突破日量/前日量 下限
MIN_BREAK = 0.03      # 突破日最小涨幅
CONC90 = 0.40         # 筹码 90% 成本集中度上限（文章写 10%，实测换手衰减后一年
                      # 窗口内筹码长尾使真实值普遍 25-40%，故默认放宽到 40%）
CHIP_BINS = 120       # 筹码分布价格档数


# ======================== 数据获取（对齐 find-retrace 惯例） ========================
def get_a_stock_codes(cursor):
    """DB 中实际存在 1d kline + fn_ 财务表的代码（fn 缺失会导致 UNION ALL 整批报错）。"""
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
    cursor.execute(r'SELECT table_name FROM information_schema.ins_tables WHERE table_name LIKE "fn\_%"')
    fn_nums = {t[0][3:] for t in cursor.fetchall()}
    return [(m, n) for m, n in a_codes if n in fn_nums]


def batch_query_kline(cursor, codes, start_date):
    queries = []
    for market, num in codes:
        queries.append(
            f"SELECT ts, open, high, low, close, volume, '{market}{num}' as code "
            f"FROM k_{market}{num}_1d WHERE ts >= '{start_date}'"
        )
    cursor.execute(" UNION ALL ".join(queries) + " ORDER BY code, ts")
    return cursor.fetchall()


def batch_query_shares(cursor, codes):
    queries = [f"SELECT '{m}{n}' as code, liutongguben FROM fn_{n}" for m, n in codes]
    cursor.execute(" UNION ALL ".join(queries))
    return {r[0]: r[1] for r in cursor.fetchall()}


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


# ======================== 筹码分布 / F10 股东 / 指数 ========================
def chip_metrics(rows, shares, price, nbins=CHIP_BINS):
    """换手率衰减筹码分布（对齐通达信筹码算法的简化版）。

    逐日：存量 ×(1-当日换手)，当日成交量均摊到 [low,high] 价格带。
    → (90%成本集中度, 套牢盘比例[现价上方筹码], 获利盘比例)。
    shares 缺失时换手按 2%/日 常数衰减（退化为时间衰减）。
    """
    import numpy as np
    w = rows[-SCAN_DAYS:]
    lo = min(float(r[3]) for r in w)
    hi = max(float(r[2]) for r in w)
    if lo >= hi or lo <= 0:
        return None
    edges = np.linspace(lo, hi, nbins + 1)
    width = edges[1] - edges[0]
    centers = (edges[:-1] + edges[1:]) / 2
    dist = np.zeros(nbins)
    for r in w:
        vol = float(r[5])
        t = min(vol / shares, 1.0) if shares and shares > 0 else 0.02
        dist *= (1.0 - t)
        i0 = int((float(r[3]) - lo) / width)
        i1 = max(i0 + 1, int((float(r[2]) - lo) / width) + 1)
        dist[i0:i1] += vol / (i1 - i0)
    total = dist.sum()
    if total <= 0:
        return None
    cdf = np.cumsum(dist) / total
    def pct(p):
        return float(np.interp(p, cdf, centers))
    p10, p50, p90 = pct(0.10), pct(0.50), pct(0.90)
    conc90 = (p90 - p10) / p50 if p50 > 0 else 9.9
    trapped = float(dist[centers > price].sum() / total)
    return conc90, trapped, 1.0 - trapped


def parse_holder_counts(text):
    """F10 股东研究「股东人数变化」表 → [(日期, 户数, 变动比率%), ...] 最新在前。

    行样例: │2026-03-31│      151091│       -9722│       -6.05│ ...
    """
    import re
    out = []
    for m in re.finditer(
            r"│(\d{4}-\d{2}-\d{2})│\s*([\d,]+)│\s*(-?[\d,]+)│\s*(-?[\d.]+)│", text):
        out.append((m.group(1), int(m.group(2).replace(",", "")), float(m.group(4))))
    return out


def fetch_holder_counts(conn, code):
    """查 F10 股东研究文本并解析户数表；缺数据返回 None。"""
    try:
        num = code[2:]
        r = conn.query(
            "SELECT content FROM f10_text WHERE code='" + num +
            "' AND cat_name='股东研究'")
        text = "".join(x[0] for x in r)
    except Exception:
        return None
    rows = parse_holder_counts(text)
    return rows[0] if rows else None


def load_index_closes(cursor, start_date):
    """上证指数日收盘 → {日期str: close}（抗跌性基准）。"""
    try:
        cursor.execute(
            f"SELECT ts, close FROM k_sh000001_1d WHERE ts >= '{start_date}'")
        return {r[0].strftime("%Y-%m-%d"): float(r[1]) for r in cursor.fetchall()}
    except Exception:
        return {}


# ======================== 筛选逻辑 ========================
def screen_stock(rows, shares, index_closes, cfg):
    """rows: (ts,O,H,L,C,V,code) 前复权升序。突破 bar = 最后一根。

    Returns: (passed, details | {"reason": ...})
    """
    if len(rows) < cfg["plat_days"] + 10:
        return False, {"reason": "数据不足"}

    closes = [float(r[4]) for r in rows]
    vols = [float(r[5]) for r in rows]
    brk_c, brk_v = closes[-1], vols[-1]

    # 8. 突破（先判触发，未触发占绝大多数，快速淘汰）
    if brk_v < cfg["vol_ratio"] * vols[-2]:
        return False, {"reason": "未放量"}
    gain = brk_c / closes[-2] - 1
    if gain < cfg["min_break"]:
        return False, {"reason": "涨幅不足"}

    # 1. 平台（截至突破前一日）；振幅为参考因子（紧致★标注），不作淘汰
    plat = rows[-1 - cfg["plat_days"]:-1]
    p_hi, p_lo = max(float(r[2]) for r in plat), min(float(r[3]) for r in plat)
    amp = (p_hi - p_lo) / p_lo if p_lo > 0 else 9.9
    plat_tight = amp <= cfg["plat_amp"]
    if brk_c < p_hi:
        return False, {"reason": "未破平台上沿"}

    # 2. 低位（一年最低 low）
    low_1y = min(float(r[3]) for r in rows[-SCAN_DAYS:])
    p_closes = sorted(float(r[4]) for r in plat)
    mid = p_closes[len(p_closes) // 2]  # 平台收盘中位数
    near_low = mid / low_1y - 1 if low_1y > 0 else 9.9
    if near_low > cfg["near_low"]:
        return False, {"reason": f"平台位置偏高 {near_low:.0%}"}

    # 3. 地量（安静期均量 vs 平台峰值量）
    plat_peak_v = max(vols[-1 - cfg["plat_days"]:-1])
    quiet_v = sum(vols[-1 - cfg["quiet_days"]:-1]) / cfg["quiet_days"]
    vol_ratio_q = quiet_v / plat_peak_v if plat_peak_v > 0 else 9.9
    if vol_ratio_q > cfg["vol_shrink"]:
        return False, {"reason": f"未缩至地量 {vol_ratio_q:.0%}"}

    # 4. 换手（有流通股本才判）
    turnover = None
    turn_slope = None
    if shares and shares > 0:
        turnover = quiet_v / shares * 100
        if turnover > cfg["max_turnover"]:
            return False, {"reason": f"换手偏高 {turnover:.1f}%"}
        # 5a. 情绪: 平台期换手率线性斜率（散户离场 → 持续萎缩）
        import numpy as np
        tos = np.array([float(r[5]) / shares * 100 for r in plat])
        turn_slope = float(np.polyfit(np.arange(len(tos)), tos, 1)[0])
        if turn_slope > 0:
            return False, {"reason": "换手未萎缩"}

    # 6. 筹码分布（换手衰减）
    chips = chip_metrics(rows, shares, brk_c)
    if chips is None:
        return False, {"reason": "筹码分布退化"}
    conc90, trapped, _profit = chips
    if conc90 > cfg["conc90"]:
        return False, {"reason": f"筹码分散 {conc90:.0%}"}

    # 5b. 情绪: 抗跌性（大盘下跌日的平均超额收益，信息列）
    defend = None
    if index_closes:
        excess = []
        for i in range(len(plat) - 1, 0, -1):
            d = plat[i][0].strftime("%Y-%m-%d")
            d_prev = plat[i - 1][0].strftime("%Y-%m-%d")
            ic, icp = index_closes.get(d), index_closes.get(d_prev)
            if ic and icp and ic < icp:  # 指数下跌日
                stock_ret = float(plat[i][4]) / float(plat[i - 1][4]) - 1
                excess.append(stock_ret - (ic / icp - 1))
        if excess:
            defend = sum(excess) / len(excess)

    return True, {
        "break_date": rows[-1][0].strftime("%Y-%m-%d"),
        "gain": gain,
        "vol_x": brk_v / vols[-2] if vols[-2] > 0 else 0,
        "plat_top": p_hi,
        "plat_amp": amp,
        "plat_tight": plat_tight,
        "conc90": conc90,
        "trapped": trapped,
        "defend": defend,
        "quiet_ratio": vol_ratio_q,
        "near_low": near_low,
        "turnover": turnover,
        "turn_slope": turn_slope,
        "close": brk_c,
    }


# ======================== 回测 ========================
def sliding_events(code, rows, shares, index_closes, cfg):
    """回测: 滑窗逐日判定「平台+地量+放量突破」，事件日 = 突破 bar（第 i 根）。

    与当日筛选共用 screen_stock; F10 无历史数据, 回测跳过（当日模式 h is None 亦放行）。
    """
    closes = [r[4] for r in rows]
    lows = [r[3] for r in rows]
    evs = []
    n = len(rows)
    for i in range(cfg["plat_days"] + 10, n):
        ok, _ = screen_stock(rows[: i + 1], shares, index_closes, cfg)
        if ok:
            evs.append({"code": code, "date": rows[i][0].strftime("%Y-%m-%d"),
                        **forward_ret(closes, lows, i)})
    return evs


# ======================== 自检 ========================
def self_test():
    cfg = dict(plat_days=60, plat_amp=0.20, near_low=0.35, quiet_days=5,
               vol_shrink=0.5, max_turnover=3.0, vol_ratio=2.0, min_break=0.03,
               conc90=0.10)

    def mk(i, o, h, l, c, v):
        return (datetime(2026, 1, 1) + timedelta(days=i), o, h, l, c, v, "sh600000")

    # 平台 70 日 9.8-10.6 横盘（振幅 8%, 一年低即平台低）→ 地量 5 日 → 放量突破
    rows = [mk(i, 10.0, 10.6, 9.8, 10.2, 1000) for i in range(70)]
    rows += [mk(70 + i, 10.0, 10.3, 9.9, 10.1, 400) for i in range(5)]    # 地量收敛
    rows.append(mk(75, 10.3, 11.6, 10.3, 11.5, 2200))                     # 放量突破: +13.9%, 量×5.5

    ok, d = screen_stock(rows, 1e8, {}, cfg)
    assert ok, d
    assert abs(d["gain"] - (11.5 / 10.1 - 1)) < 1e-9 and d["vol_x"] == 5.5, d
    assert d["plat_top"] == 10.6 and d["conc90"] < 0.10 and d["plat_tight"], d  # 单峰密集+紧致★
    assert d["trapped"] < 0.10, d  # 现价上方套牢盘极少

    # 未放量 → 淘汰
    rows[-1] = mk(75, 10.3, 11.6, 10.3, 11.5, 500)
    ok, d = screen_stock(rows, 1e8, {}, cfg)
    assert not ok and d["reason"] == "未放量", d
    # 未破平台上沿（收 10.5 < 10.6）
    rows[-1] = mk(75, 10.3, 10.8, 10.2, 10.5, 2200)
    ok, d = screen_stock(rows, 1e8, {}, cfg)
    assert not ok and d["reason"] == "未破平台上沿", d
    # 平台振幅为参考因子: 下探 8.5 → 振幅 ~25% 仍通过, 但不标紧致★
    rows2 = list(rows)
    rows2[-2] = mk(74, 10.0, 10.3, 8.5, 8.8, 400)
    rows2[-1] = mk(75, 10.3, 11.6, 10.3, 11.5, 2200)
    ok, d = screen_stock(rows2, 1e8, {}, cfg)
    assert ok and not d["plat_tight"], d
    # 未缩至地量（安静期放量 900 vs 峰值 1000）
    rows3 = list(rows)
    for i in range(5):
        rows3[-2 - i] = mk(74 - i, 10.0, 10.3, 9.9, 10.1, 900)
    rows3[-1] = mk(75, 10.3, 11.6, 10.3, 11.5, 2200)
    ok, d = screen_stock(rows3, 1e8, {}, cfg)
    assert not ok and d["reason"].startswith("未缩至地量"), d
    # 换手偏高: 流通股 1e4 股, 400 量 → 4%/日
    rows[-1] = mk(75, 10.3, 11.6, 10.3, 11.5, 2200)
    ok, d = screen_stock(rows, 1e4, {}, cfg)
    assert not ok and d["reason"].startswith("换手偏高"), d
    # 换手未萎缩: 平台量线性放大（斜率>0），但安静期仍为峰值一半以下
    rows4 = [mk(i, 10.0, 10.6, 9.8, 10.2, 100 + i * 13) for i in range(70)]
    rows4 += [mk(70 + i, 10.0, 10.3, 9.9, 10.1, 400) for i in range(5)]
    rows4.append(mk(75, 10.3, 11.6, 10.3, 11.5, 2200))
    ok, d = screen_stock(rows4, 1e6, {}, cfg)
    assert not ok and d["reason"] == "换手未萎缩", d
    # 筹码分散: 前半历史 8.5 元 + 后半 10 元（无衰减, 两峰; 低位条件仍满足）
    rows5 = [mk(i, 8.6, 8.8, 8.5, 8.7, 1000) for i in range(40)]
    rows5 += [mk(40 + i, 10.0, 10.6, 9.8, 10.2, 1000) for i in range(70)]
    rows5 += [mk(110 + i, 10.0, 10.3, 9.9, 10.1, 400) for i in range(5)]
    rows5.append(mk(115, 10.3, 11.6, 10.3, 11.5, 2200))
    ok, d = screen_stock(rows5, 1e12, {}, cfg)
    assert not ok and d["reason"].startswith("筹码分散"), d

    # 筹码分布单元: 单一价带 → conc90 小; 双峰 → 大
    one = [mk(i, 10.0, 10.1, 10.0, 10.0, 1000) for i in range(50)]
    assert chip_metrics(one, 1e12, 10.0)[0] < 0.05, "单峰集中"
    two = one[:25] + [mk(25 + i, 20.0, 20.1, 20.0, 20.0, 1000) for i in range(25)]
    assert chip_metrics(two, 1e12, 20.0)[0] > 0.4, "双峰分散"
    assert chip_metrics(two, 1e12, 20.1)[1] < 0.02, "现价≥上峰顶 → 套牢盘≈0"

    # F10 股东人数解析（真实文本样例）
    f10 = ("│截止日期  │股东人数(户)│变动户数(户)│ 变动比率(%)│\r\n"
           "│2026-03-31│      151091│       -9722│       -6.05│\r\n"
           "│2026-02-28│      160813│       38486│       31.46│\r\n")
    holders = parse_holder_counts(f10)
    assert holders[0] == ("2026-03-31", 151091, -6.05), holders
    assert holders[1][2] == 31.46, holders
    print("self-test OK")


# ======================== 主函数 ========================
def main():
    parser = argparse.ArgumentParser(description="吸筹结束突破筛选 — 券商吸筹标准量化版")
    parser.add_argument("--all", action="store_true", help="全 A 股 (默认仅自选股 zxg.blk)")
    parser.add_argument("--plat-days", type=int, default=PLAT_DAYS, help="平台窗口交易日 (默认 60)")
    parser.add_argument("--plat-amp", type=float, default=PLAT_AMP, help="平台最大振幅 (默认 0.20)")
    parser.add_argument("--near-low", type=float, default=NEAR_LOW, help="平台中位价距一年最低上限 (默认 0.35)")
    parser.add_argument("--quiet-days", type=int, default=QUIET_DAYS, help="地量窗口 (默认 5)")
    parser.add_argument("--vol-shrink", type=float, default=VOL_SHRINK, help="地量/平台峰值量上限 (默认 0.5)")
    parser.add_argument("--max-turnover", type=float, default=MAX_TURNOVER, help="安静期日均换手%%上限 (默认 3)")
    parser.add_argument("--vol-ratio", type=float, default=VOL_RATIO, help="突破日量/前日量下限 (默认 2.0)")
    parser.add_argument("--min-break", type=float, default=MIN_BREAK, help="突破日最小涨幅 (默认 0.03)")
    parser.add_argument("--conc90", type=float, default=CONC90, help="筹码90%%成本集中度上限 (默认 0.40)")
    parser.add_argument("--output-dir", default=os.path.join(OUTPUT_DIR, "find-finish-eating"), help="Excel 输出目录")
    parser.add_argument("--self-test", action="store_true", help="运行内置自检后退出")
    parser.add_argument("--market-bull", action="store_true",
                        help="大盘空头时跳过筛选（默认仅标注，不拦截）")
    parser.add_argument("--backtest", action="store_true",
                        help="滑窗回测: 逐日信号→前瞻收益(+5/20/65)汇总")
    args = parser.parse_args()

    if args.self_test:
        self_test()
        return 0

    cfg = dict(plat_days=args.plat_days, plat_amp=args.plat_amp, near_low=args.near_low,
               quiet_days=args.quiet_days, vol_shrink=args.vol_shrink,
               max_turnover=args.max_turnover, vol_ratio=args.vol_ratio,
               min_break=args.min_break, conc90=args.conc90)

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
    db_codes = set(get_a_stock_codes(cursor))
    if args.all:
        pool = set(all_mainboard_codes(conn))
        pool_desc = f"全 A 股 {len(pool)} 只"
    else:
        pool = {parse_code(c) for c in zxg_codes()}
        pool_desc = f"自选股 zxg.blk {len(pool)} 只"
    a_stocks = sorted(db_codes & pool)
    n_st = sum(1 for mc in a_stocks
               if is_st_name(names_by_code.get(f"{mc[0]}{mc[1]}", "")))
    a_stocks = [mc for mc in a_stocks
                if not is_st_name(names_by_code.get(f"{mc[0]}{mc[1]}", ""))]
    if not a_stocks:
        sys.stderr.write("无候选标的（加 --all 筛全市场）\n")
        return 1
    adj_by_mc = batch_fetch_adjust(conn, a_stocks)
    print(f"[1/4] {pool_desc} ∩ DB = {len(a_stocks)} 只"
          f"(排除 ST/*ST {n_st}), 复权事件 {len(adj_by_mc)} 只 "
          f"(耗时 {time.time()-t0:.1f}s)", file=sys.stderr)

    # 回测需更长历史: 60 日平台 + 250 日一年低 + 评估期
    bt_days = SCAN_DAYS * 3 // 2 + 700 if args.backtest else SCAN_DAYS * 3 // 2 + 30
    start_date = (datetime.now() - timedelta(days=bt_days)).strftime("%Y-%m-%d")
    index_closes = load_index_closes(cursor, start_date)
    results = []
    bt_events = []
    reject = defaultdict(int)
    total_batches = (len(a_stocks) + BATCH_SIZE - 1) // BATCH_SIZE

    for bi in range(total_batches):
        batch = a_stocks[bi * BATCH_SIZE:(bi + 1) * BATCH_SIZE]
        t2 = time.time()
        kline_rows = batch_query_kline(cursor, batch, start_date)
        shares = batch_query_shares(cursor, batch)
        stock_data = defaultdict(list)
        for row in kline_rows:
            stock_data[row[6]].append(row)

        n_pass = 0
        for market, num in batch:
            code = f"{market}{num}"
            rows = qfq_rows(stock_data.get(code, []), adj_by_mc.get((market, num)))
            if not rows:
                continue
            if args.backtest:
                bt_events.extend(sliding_events(code, rows, shares.get(code),
                                                index_closes, cfg))
                continue
            passed, d = screen_stock(rows, shares.get(code), index_closes, cfg)
            if passed:
                n_pass += 1
                d["name"] = names_by_code.get(code, "")
                results.append((code, d))
            else:
                reject[d["reason"].split()[0] if "平台过宽" in d["reason"] else d["reason"]] += 1
        print(f"[2/4] 批次 {bi+1}/{total_batches}: {len(batch)} 只, 通过 {n_pass}, "
              f"kline {time.time()-t2:.1f}s", file=sys.stderr)

    if args.backtest:
        conn.close()
        mode_desc = "全 A 股" if args.all else "自选股"
        print(f"[backtest] 共 {len(bt_events)} 个事件", file=sys.stderr)
        report_events(bt_events, title=f"find-finish-eating 回测 ({mode_desc})")
        return 0

    # F10 股东户数（漏斗末端，仅对幸存者逐只懒查；最新期户数减少为条件）
    kept = []
    for code, d in results:
        h = fetch_holder_counts(conn, code)
        if h is None:
            d["holder_date"], d["holders"], d["holder_chg"] = "", None, None
        else:
            d["holder_date"], d["holders"], d["holder_chg"] = h
        if h is not None and h[2] >= 0:
            print(f"[F10] {code} 户数未减少 ({h[0]}: {h[2]:+.1f}%)，剔除", file=sys.stderr)
            continue
        kept.append((code, d))
    results = kept

    conn.close()
    results.sort(key=lambda x: x[1]["gain"], reverse=True)
    print(f"[3/4] 筛选完成: {len(results)} 只通过 (总耗时 {time.time()-t0:.1f}s)", file=sys.stderr)
    for reason, cnt in sorted(reject.items(), key=lambda x: -x[1]):
        print(f"       淘汰 {reason}: {cnt}", file=sys.stderr)
    print("       提示: 盘中运行当日量未走满会低估量比，建议尾盘运行", file=sys.stderr)

    print(f"\n{'='*112}")
    print(f" 吸筹结束突破筛选 [{pool_desc}] 突破bar=最新一根")
    print(f" 条件: 平台{args.plat_days}日(振幅参考, <={args.plat_amp:.0%}标★), 距一年低<={args.near_low:.0%}, "
          f"地量<={args.vol_shrink:.0%}×峰值, 换手<={args.max_turnover:.0f}%, "
          f"突破量>={args.vol_ratio:.0f}×前日 涨幅>={args.min_break:.0%}")
    print(f"{'='*112}")
    print(f"{'代码':<10} {'名称':<10} {'突破日':<12} {'涨幅':>6} {'量比':>5} "
          f"{'平台上沿':>8} {'平台振幅':>9} {'90%集中':>7} {'套牢':>5} {'地量比':>6} "
          f"{'距一年低':>7} {'换手%':>6} {'抗跌':>6} {'户数截止':<11} {'户数':>8} {'户数变动':>7}")
    print("-" * 130)
    for code, d in results:
        defend = f"{d['defend']:>5.1%}" if d["defend"] is not None else "    --"
        holders = f"{d['holders']:>8d}" if d["holders"] else "      --"
        hchg = f"{d['holder_chg']:>+6.1f}%" if d["holder_chg"] is not None else "    --"
        amp_str = f"{d['plat_amp']:>6.1%}{'★' if d['plat_tight'] else ' '}"
        print(f"{code:<10} {d['name']:<10} {d['break_date']:<12} {d['gain']:>5.1%} "
              f"{d['vol_x']:>4.1f}x {d['plat_top']:>8.2f} {amp_str} "
              f"{d['conc90']:>6.1%} {d['trapped']:>4.0%} {d['quiet_ratio']:>5.0%} "
              f"{d['near_low']:>6.0%} "
              f"{(d['turnover'] if d['turnover'] is not None else float('nan')):>6.1f} "
              f"{defend} {d['holder_date'] or '--':<11} {holders} {hchg}")
    print(f"{'='*130}")

    os.makedirs(args.output_dir, exist_ok=True)
    rows = [{
        "代码": code, "名称": d["name"], "突破日": d["break_date"],
        "涨幅%": round(d["gain"] * 100, 1), "量比": round(d["vol_x"], 1),
        "平台上沿": d["plat_top"], "平台振幅%": round(d["plat_amp"] * 100, 1),
        "平台紧致": "是" if d["plat_tight"] else "否",
        "90%集中%": round(d["conc90"] * 100, 1), "套牢%": round(d["trapped"] * 100, 1),
        "地量比%": round(d["quiet_ratio"] * 100),
        "距一年低%": round(d["near_low"] * 100), "换手%": round(d["turnover"], 2) if d["turnover"] else None,
        "抗跌超额%": round(d["defend"] * 100, 1) if d["defend"] is not None else None,
        "户数截止": d["holder_date"] or None, "股东户数": d["holders"],
        "户数变动%": d["holder_chg"],
        "现价": d["close"],
    } for code, d in results]
    stamped = os.path.join(args.output_dir, f"find-finish-eating-{time.strftime('%Y%m%d')}.xlsx")
    pd.DataFrame(rows).to_excel(stamped, index=False)
    print(f"[4/4] [xlsx] → {stamped} (共 {len(rows)} 条)", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
