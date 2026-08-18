#!/usr/bin/env python3
"""
================================================================================
 底部反转筛选脚本 — 600105 标准股份模式
================================================================================

筛选与 600105 类似的「恐慌长针探底 + 假阴线洗盘 + 高换手确认」底部反转标的。

筛选条件（基于 600105 实证特征）:
  1. 近期大幅回撤（60日内从高点回撤 > 40%）
  2. 底部日长下影锤头（振幅 > 8%，下影比例 > 50%）
  3. 底部后 3 日内出现假阴线（open > close 但 close > prev_close）
  4. 底部 5 日内换手率峰值 > 15%
  5. 二次探底不破前低（可选）
  6. 突破确认：最新收盘 > 底部日开盘（可选）

用法:
  python3 scripts/find-reversal.py                    # 默认筛自选股 zxg.blk
  python3 scripts/find-reversal.py --all              # 筛全部 A 股
  python3 scripts/find-reversal.py --min-drawdown 0.35 --min-turnover 12
  python3 scripts/find-reversal.py --days 45 --top 20
================================================================================
"""

import json
import os
import sys
import time
from collections import defaultdict
from datetime import datetime, timedelta

import pandas as pd
import taosws

from common import OUTPUT_DIR, all_mainboard_codes, parse_code, zxg_codes


# ======================== 配置 ========================
TDENGINE_HOST = os.environ.get("TDENGINE_HOST", "localhost")
TDENGINE_PORT = int(os.environ.get("TDENGINE_PORT", "6041"))
TDENGINE_USER = os.environ.get("TDENGINE_USER", "root")
TDENGINE_PASS = os.environ.get("TDENGINE_PASS", "taosdata")
TDENGINE_DB = os.environ.get("TDENGINE_DB", "tdx")

BATCH_SIZE = 1800  # UNION ALL 批大小（实测 ~1.8s/批）
LOOKBACK_DAYS = 60  # 回看天数
MIN_DRAWDOWN = 0.40  # 最小回撤幅度
MIN_BOTTOM_AMPLITUDE = 0.08  # 底部日最小振幅
MIN_LOWER_SHADOW_RATIO = 0.50  # 最小下影比例
MIN_TURNOVER_PEAK = 15.0  # 最小换手率峰值
REQUIRE_FAKE_BEARISH = True  # 是否要求假阴线
REQUIRE_SECONDARY_TEST = True  # 是否要求二次探底不破
REQUIRE_BREAKOUT = True  # 是否要求突破确认
TOP_N = 50  # 输出前 N 个结果


# ======================== 数据获取 ========================
def get_a_stock_codes(cursor):
    """DB 中实际存在 1d kline + fn_ 财务表的代码 → [(market, code), ...]。

    用于保证 UNION ALL 批量查询不因缺表报错。可能含少量指数表（如 sz399xxx），
    由 main 中与 all_mainboard_codes/zxg 的交集兜底排除。
    """
    cursor.execute(r'SELECT table_name FROM information_schema.ins_tables WHERE table_name LIKE "k\_%\_1d"')
    tables = cursor.fetchall()

    a_codes = []
    for t in tables:
        name = t[0]
        inner = name[2:-3]  # 去掉 "k_" 前缀和 "_1d" 后缀
        market = inner[:2]
        num = inner[2:]
        if len(num) == 6 and num.isdigit():
            if (market == "sh" and num[0] == "6") or \
               (market == "sz" and num[0] in "03") or \
               (market == "bj" and num[0] in "48"):
                a_codes.append((market, num))

    # 过滤有 fn_ 数据的
    cursor.execute(r'SELECT table_name FROM information_schema.ins_tables WHERE table_name LIKE "fn\_%"')
    fn_tables = cursor.fetchall()
    fn_nums = {t[0][3:] for t in fn_tables}

    return [(m, n) for m, n in a_codes if n in fn_nums]


def batch_query_kline(cursor, codes, start_date):
    """批量查询日线数据"""
    queries = []
    for market, num in codes:
        queries.append(
            f"SELECT ts, open, high, low, close, volume, '{market}{num}' as code "
            f"FROM k_{market}{num}_1d WHERE ts >= '{start_date}'"
        )
    full_query = " UNION ALL ".join(queries) + " ORDER BY code, ts"
    cursor.execute(full_query)
    return cursor.fetchall()


def batch_query_shares(cursor, codes):
    """批量查询流通股本"""
    queries = []
    for market, num in codes:
        queries.append(f"SELECT '{market}{num}' as code, liutongguben FROM fn_{num}")
    full_query = " UNION ALL ".join(queries)
    cursor.execute(full_query)
    shares = {}
    for row in cursor.fetchall():
        shares[row[0]] = row[1]
    return shares


def load_stock_names(conn):
    """从 stock_name 表加载 {code: 名称}（code 形如 'sh600105'）。"""
    try:
        return {f"{m}{c}": n for m, c, n in conn.query(
            "SELECT market, code, name FROM tdx.stock_name")}
    except Exception:
        return {}


def write_xlsx(all_results, output_dir):
    """结果写 Excel：带日期戳 + 固定最新名双文件（对齐 find-diverse 惯例）。

    百分比列 ×100 存数值（如 回撤 55.1），列名带 %，Excel 直读无需设格式。
    """
    os.makedirs(output_dir, exist_ok=True)
    rows = []
    for code, d in all_results:
        rows.append({
            "代码": code,
            "名称": d.get("name", ""),
            "底部日期": d["trough_date"],
            "低点": d["trough_low"],
            "回撤%": round(d["drawdown"] * 100, 1),
            "振幅%": round(d["bottom_amplitude"] * 100, 1),
            "下影%": round(d["lower_shadow_ratio"] * 100, 1),
            "假阴线": ",".join(d["fake_bearish"]) if d["fake_bearish"] else "",
            "换手%": round(d["turnover_peak"], 1),
            "换手日": d.get("turnover_peak_date", ""),
            "二次低": d["secondary_low"] if d["secondary_low"] is not None else "",
            "现价": d["current_close"],
            "反弹%": round(d["rebound"] * 100, 1),
        })
    df = pd.DataFrame(rows)
    stamp = time.strftime("%Y%m%d")
    stamped = os.path.join(output_dir, f"find-reversal-{stamp}.xlsx")
    df.to_excel(stamped, index=False)
    return stamped, len(rows)


# ======================== 筛选逻辑 ========================
def screen_stock(code, kline_rows, float_shares, config=None):
    """
    对单只股票应用筛选条件。

    Returns: (passed: bool, details: dict)
    """
    if config is None:
        config = {}
    min_drawdown = config.get("min_drawdown", MIN_DRAWDOWN)
    min_amplitude = config.get("min_amplitude", MIN_BOTTOM_AMPLITUDE)
    min_turnover = config.get("min_turnover", MIN_TURNOVER_PEAK)
    req_fake = config.get("req_fake_bearish", REQUIRE_FAKE_BEARISH)
    req_secondary = config.get("req_secondary_test", REQUIRE_SECONDARY_TEST)
    req_breakout = config.get("req_breakout", REQUIRE_BREAKOUT)

    if len(kline_rows) < 20:
        return False, {"reason": "数据不足"}

    # 解析数据
    dates = []
    opens = []
    highs = []
    lows = []
    closes = []
    volumes = []

    for row in kline_rows:
        ts, o, h, l, c, v, _ = row
        dates.append(ts)
        opens.append(float(o))
        highs.append(float(h))
        lows.append(float(l))
        closes.append(float(c))
        volumes.append(float(v))

    n = len(closes)

    # 1. 计算 60 日回撤
    peak_high = max(highs)
    trough_low = min(lows)
    drawdown = (peak_high - trough_low) / peak_high if peak_high > 0 else 0

    if drawdown < min_drawdown:
        return False, {"reason": f"回撤不足 {drawdown:.1%} < {min_drawdown:.0%}"}

    # 找到最低点日期
    trough_idx = lows.index(trough_low)
    trough_date = dates[trough_idx]

    # 2. 底部日长下影锤头检查
    bottom_o = opens[trough_idx]
    bottom_h = highs[trough_idx]
    bottom_l = lows[trough_idx]
    bottom_c = closes[trough_idx]
    bottom_amplitude = (bottom_h - bottom_l) / bottom_l if bottom_l > 0 else 0
    lower_shadow = bottom_c - bottom_l
    amplitude_range = bottom_h - bottom_l
    lower_shadow_ratio = lower_shadow / amplitude_range if amplitude_range > 0 else 0

    if bottom_amplitude < min_amplitude:
        return False, {"reason": f"底部振幅不足 {bottom_amplitude:.1%}"}

    if lower_shadow_ratio < MIN_LOWER_SHADOW_RATIO:
        return False, {"reason": f"下影比例不足 {lower_shadow_ratio:.1%}"}

    # 3. 假阴线检查（底部后 3 日内）
    fake_bearish_dates = []
    for i in range(trough_idx + 1, min(trough_idx + 4, n)):
        if i > 0:
            prev_c = closes[i - 1]
            curr_o = opens[i]
            curr_c = closes[i]
            if curr_o > curr_c and curr_c > prev_c:
                fake_bearish_dates.append(dates[i].strftime("%m-%d"))

    if req_fake and not fake_bearish_dates:
        return False, {"reason": "无假阴线"}

    # 4. 换手率检查（底部 5 日内）
    if float_shares and float_shares > 0:
        turnover_peak = 0
        turnover_peak_date = ""
        for i in range(max(0, trough_idx), min(trough_idx + 5, n)):
            to = volumes[i] / float_shares * 100
            if to > turnover_peak:
                turnover_peak = to
                turnover_peak_date = dates[i].strftime("%m-%d")

        if turnover_peak < min_turnover:
            return False, {"reason": f"换手率不足 {turnover_peak:.1f}%"}
    else:
        turnover_peak = 0
        turnover_peak_date = ""

    # 5. 二次探底不破前低
    secondary_test_pass = False
    secondary_low = None
    if trough_idx < n - 1:
        remaining_lows = lows[trough_idx + 1:]
        if remaining_lows:
            secondary_low = min(remaining_lows)
            secondary_test_pass = secondary_low > trough_low

    if req_secondary and not secondary_test_pass:
        return False, {"reason": "二次探底破前低"}

    # 6. 突破确认
    breakout_pass = closes[-1] > bottom_o if bottom_o > 0 else False

    if req_breakout and not breakout_pass:
        return False, {"reason": "未突破底部日开盘"}

    # 计算反弹幅度
    rebound = (closes[-1] - trough_low) / trough_low if trough_low > 0 else 0

    details = {
        "drawdown": drawdown,
        "trough_date": trough_date.strftime("%Y-%m-%d"),
        "trough_low": trough_low,
        "bottom_close": bottom_c,
        "bottom_amplitude": bottom_amplitude,
        "lower_shadow_ratio": lower_shadow_ratio,
        "fake_bearish": fake_bearish_dates,
        "turnover_peak": turnover_peak,
        "turnover_peak_date": turnover_peak_date,
        "secondary_low": secondary_low,
        "secondary_test_pass": secondary_test_pass,
        "current_close": closes[-1],
        "rebound": rebound,
        "breakout": breakout_pass,
    }

    return True, details


# ======================== 主函数 ========================
def main():
    import argparse
    parser = argparse.ArgumentParser(description="底部反转筛选 — 600105 模式")
    parser.add_argument("--all", action="store_true", help="筛全部 A 股 (默认仅自选股 zxg.blk)")
    parser.add_argument("--min-drawdown", type=float, default=MIN_DRAWDOWN, help="最小回撤幅度")
    parser.add_argument("--min-amplitude", type=float, default=MIN_BOTTOM_AMPLITUDE, help="底部日最小振幅")
    parser.add_argument("--min-turnover", type=float, default=MIN_TURNOVER_PEAK, help="最小换手率峰值")
    parser.add_argument("--days", type=int, default=LOOKBACK_DAYS, help="回看天数")
    parser.add_argument("--top", type=int, default=TOP_N, help="输出前 N 个结果")
    parser.add_argument("--no-fake-bearish", action="store_true", help="不要求假阴线")
    parser.add_argument("--no-secondary-test", action="store_true", help="不要求二次探底")
    parser.add_argument("--no-breakout", action="store_true", help="不要求突破确认")
    parser.add_argument("--json", action="store_true", help="JSON 输出 (stdout, 不写 xlsx)")
    parser.add_argument("--output-dir", default=os.path.join(OUTPUT_DIR, "find-reversal"), help="Excel 输出目录")
    args = parser.parse_args()

    # 构建配置
    config = {
        "min_drawdown": args.min_drawdown,
        "min_amplitude": args.min_amplitude,
        "min_turnover": args.min_turnover,
        "req_fake_bearish": not args.no_fake_bearish,
        "req_secondary_test": not args.no_secondary_test,
        "req_breakout": not args.no_breakout,
    }

    # 连接数据库
    url = f"taosws://{TDENGINE_USER}:{TDENGINE_PASS}@{TDENGINE_HOST}:{TDENGINE_PORT}/{TDENGINE_DB}"
    conn = taosws.connect(url)
    cursor = conn.cursor()
    names_by_code = load_stock_names(conn)

    t0 = time.time()

    # DB 中实际有 kline+fn 表的代码（保证 UNION ALL 批量查询不因缺表报错）
    db_codes = set(get_a_stock_codes(cursor))
    t1 = time.time()

    # 标的池：--all 全 A 股（stock_name 表，排除指数）；默认自选股 zxg.blk
    if args.all:
        pool = set(all_mainboard_codes(conn))
        pool_desc = f"全 A 股(stock_name) {len(pool)} 只"
    else:
        pool = {parse_code(c) for c in zxg_codes()}
        pool_desc = f"自选股 zxg.blk {len(pool)} 只"

    # 交集：既有规范标的身份、又有实际数据
    a_stocks = sorted(db_codes & pool)
    print(f"[1/4] 标的池: {pool_desc} ∩ DB有数据 = {len(a_stocks)} 只 (耗时 {t1-t0:.2f}s)",
          file=sys.stderr)
    if not a_stocks:
        sys.stderr.write("无候选标的（自选股为空或无数据，加 --all 筛全市场）\n")
        return 1

    # 计算起始日期
    start_date = (datetime.now() - timedelta(days=args.days + 10)).strftime("%Y-%m-%d")

    # 批量查询
    all_results = []
    total_batches = (len(a_stocks) + BATCH_SIZE - 1) // BATCH_SIZE

    for batch_idx in range(total_batches):
        batch_start = batch_idx * BATCH_SIZE
        batch_end = min(batch_start + BATCH_SIZE, len(a_stocks))
        batch = a_stocks[batch_start:batch_end]

        # 查询 kline
        t2 = time.time()
        kline_rows = batch_query_kline(cursor, batch, start_date)
        t3 = time.time()

        # 查询 shares
        shares = batch_query_shares(cursor, batch)
        t4 = time.time()

        # 按股票分组
        stock_data = defaultdict(list)
        for row in kline_rows:
            stock_data[row[6]].append(row)

        # 逐股筛选
        batch_passed = 0
        for market, num in batch:
            code = f"{market}{num}"
            rows = stock_data.get(code, [])
            if not rows:
                continue
            float_shares = shares.get(code)
            passed, details = screen_stock(code, rows, float_shares, config)
            if passed:
                batch_passed += 1
                details["name"] = names_by_code.get(code, "")
                all_results.append((code, details))

        print(
            f"[2/4] 批次 {batch_idx+1}/{total_batches}: "
            f"{len(batch)} 只, 通过 {batch_passed}, "
            f"kline {t3-t2:.1f}s, shares {t4-t3:.1f}s",
            file=sys.stderr,
        )

    conn.close()

    # 排序：按反弹幅度降序
    all_results.sort(key=lambda x: x[1]["rebound"], reverse=True)

    t5 = time.time()
    print(f"[3/4] 筛选完成: {len(all_results)} 只通过 (总耗时 {t5-t0:.2f}s)", file=sys.stderr)

    # 输出
    min_dd = config["min_drawdown"]
    min_amp = config["min_amplitude"]
    min_to = config["min_turnover"]
    req_fake = config["req_fake_bearish"]
    req_sec = config["req_secondary_test"]
    req_bo = config["req_breakout"]
    mode_desc = "全 A 股" if args.all else "自选股"

    if args.json:
        output = []
        for code, d in all_results[:args.top]:
            output.append({"code": code, **d})
        print(json.dumps(output, ensure_ascii=False, indent=2, default=str))
    else:
        print(f"\n{'='*100}")
        print(f" 底部反转筛选结果 — 600105 标准股份模式 [{mode_desc}]")
        print(f" 条件: 回撤>{min_dd:.0%}, 振幅>{min_amp:.0%}, "
              f"换手>{min_to:.0f}%, 假阴线={req_fake}, "
              f"二次探底={req_sec}, 突破={req_bo}")
        print(f" 共筛选 {len(a_stocks)} 只{mode_desc}, {len(all_results)} 只通过")
        print(f"{'='*100}")
        print(f"{'代码':<10} {'底部日期':<12} {'低点':>8} {'回撤':>6} {'振幅':>6} "
              f"{'下影%':>6} {'假阴线':<10} {'换手%':>6} {'二次低':>8} {'现价':>8} {'反弹%':>6}")
        print(f"{'-'*100}")

        for code, d in all_results[:args.top]:
            fake_str = ",".join(d["fake_bearish"]) if d["fake_bearish"] else "-"
            sec_low = f"{d['secondary_low']:.2f}" if d["secondary_low"] else "-"
            print(
                f"{code:<10} {d['trough_date']:<12} {d['trough_low']:>8.2f} "
                f"{d['drawdown']:>5.1%} {d['bottom_amplitude']:>5.1%} "
                f"{d['lower_shadow_ratio']:>5.1%} {fake_str:<10} "
                f"{d['turnover_peak']:>5.1f} {sec_low:>8} "
                f"{d['current_close']:>8.2f} {d['rebound']:>5.1%}"
            )

        print(f"{'='*100}")
        print(f"[4/4] 输出前 {min(args.top, len(all_results))} 个结果", file=sys.stderr)

    # 写 Excel（json 模式跳过；写全部通过结果，不限 top）
    if not args.json:
        stamped, n = write_xlsx(all_results, args.output_dir)
        print(f"[xlsx] → {stamped} (共 {n} 条)", file=sys.stderr)

    return 0


if __name__ == "__main__":
    sys.exit(main())
