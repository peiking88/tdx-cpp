#!/usr/bin/env python3
"""
================================================================================
 昨日涨停今低开筛选脚本 — 底部区域回踩模式
================================================================================

今日开盘后运行一次，筛选：昨日涨停、今日开盘下跌、当前价仍在最近底部低点
上方一定涨幅以内（默认 10%）的 A 股标的。

筛选条件（日线采用前复权，除权缺口不干扰跨日比价）:
  1. 昨日（上一交易日）收盘 = 涨停价（主板 10% / 创业板科创板 20% / 北交所 30% /
     主板 ST 5%；涨停价 = 前收 ×(1+比率) 四舍五入到分，Decimal 精确计算）
  2. 今日开盘价 < 昨日收盘（低开）
  3. (当前价 - 底部最低价) / 底部最低价 <= --rise（默认 10%）
     底部最低价 = 回看一年（250 交易日）内最低 low（不含今日），自动确定无需参数。

前置条件: 需 fetch-today / fetch-kline 已在盘中运行，当日 1d bar 已入库
（当前价取当日 bar 的 close，即最新价）。

用法:
  python3 scripts/find-retrace.py                     # 默认筛自选股 zxg.blk
  python3 scripts/find-retrace.py --all               # 筛全部 A 股
  python3 scripts/find-retrace.py --rise 0.15
  python3 scripts/find-retrace.py --all --y           # 仅昨日涨停+今日低开, 不限距底涨幅
================================================================================
"""

import json
import os
import sys
import time
from collections import Counter, defaultdict
from datetime import datetime, timedelta
from decimal import Decimal, ROUND_HALF_UP

import pandas as pd
import taosws

from common import OUTPUT_DIR, all_mainboard_codes, apply_qfq, batch_fetch_adjust, parse_code, zxg_codes


# ======================== 配置 ========================
TDENGINE_HOST = os.environ.get("TDENGINE_HOST", "localhost")
TDENGINE_PORT = int(os.environ.get("TDENGINE_PORT", "6041"))
TDENGINE_USER = os.environ.get("TDENGINE_USER", "root")
TDENGINE_PASS = os.environ.get("TDENGINE_PASS", "taosdata")
TDENGINE_DB = os.environ.get("TDENGINE_DB", "tdx")

BATCH_SIZE = 1800  # UNION ALL 批大小（实测 ~1.8s/批）
BOTTOM_SCAN_DAYS = 250  # U 型底检测回看上限（交易日，≈一年）
MAX_RISE_FROM_BOTTOM = 0.10  # 距底部低点最大涨幅
TOP_N = 50


# ======================== 涨停判定 ========================
def limit_ratio(market, num, name):
    """涨停幅度: 北交所 30% / 科创板+创业板 20% / 主板 10% / 主板 ST 5%。

    科创/创业板的 ST 仍为 20%。
    """
    if market == "bj":
        return 0.30
    if num.startswith("688") or num.startswith("689") or num.startswith("30"):
        return 0.20
    return 0.05 if "ST" in (name or "") else 0.10


def limit_up_price(prev_close, ratio):
    """涨停价 = 前收 ×(1+比率)，四舍五入到分。Decimal 精确（浮点会把
    9.95×1.1=10.945 算成 10.944999… → 10.94，错 1 分钱）。"""
    p = (Decimal(str(prev_close)) * (1 + Decimal(str(ratio)))).quantize(
        Decimal("0.01"), rounding=ROUND_HALF_UP)
    return float(p)


def is_limit_up(close, prev_close, market, num, name):
    """收盘是否封住涨停价（均为 2 位小数价格，差 <0.005 即相等）。"""
    limit = limit_up_price(prev_close, limit_ratio(market, num, name))
    return abs(close - limit) < 0.005, limit


# ======================== 数据获取 ========================
def get_a_stock_codes(cursor):
    """DB 中实际存在 1d kline 表的 A 股代码 → [(market, code), ...]。"""
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
    return a_codes


def batch_query_kline(cursor, codes, start_date):
    """批量查询日线数据（含当日盘中 bar）"""
    queries = []
    for market, num in codes:
        queries.append(
            f"SELECT ts, open, high, low, close, volume, '{market}{num}' as code "
            f"FROM k_{market}{num}_1d WHERE ts >= '{start_date}'"
        )
    full_query = " UNION ALL ".join(queries) + " ORDER BY code, ts"
    cursor.execute(full_query)
    return cursor.fetchall()


def qfq_rows(rows, events):
    """行元组前复权（借 common.apply_qfq），仅改 OHLC，ts/vol/code 不变。

    无事件的股票原样返回（绝大多数，避免全市场 df 化开销）。
    """
    if not events:
        return rows
    df = pd.DataFrame(rows, columns=["ts", "O", "H", "L", "C", "V", "code"])
    df["ts"] = pd.to_datetime(df["ts"])
    df = apply_qfq(df, events)
    return list(df.itertuples(index=False, name=None))


def load_stock_names(conn):
    """从 stock_name 表加载 {code: 名称}（code 形如 'sh600105'）。"""
    try:
        return {f"{m}{c}": n for m, c, n in conn.query(
            "SELECT market, code, name FROM tdx.stock_name")}
    except Exception:
        return {}


def write_xlsx(all_results, output_dir):
    """结果写 Excel（带日期戳，对齐 find-reversal 惯例）。百分比列 ×100 存数值。"""
    os.makedirs(output_dir, exist_ok=True)
    rows = []
    for code, d in all_results:
        rows.append({
            "代码": code,
            "名称": d.get("name", ""),
            "昨收": d["yday_close"],
            "涨停幅%": round(d["ratio"] * 100),
            "今开": d["today_open"],
            "低开%": round(d["open_drop"] * 100, 1),
            "现价": d["current"],
            "底部日期": d["bottom_date"],
            "底部低点": d["bottom_low"],
            "距底涨幅%": round(d["rise_from_bottom"] * 100, 1),
        })
    df = pd.DataFrame(rows)
    stamp = time.strftime("%Y%m%d")
    stamped = os.path.join(output_dir, f"find-retrace-{stamp}.xlsx")
    df.to_excel(stamped, index=False)
    return stamped, len(rows)


# ======================== 筛选逻辑 ========================
def find_recent_bottom(hist, scan_days=BOTTOM_SCAN_DAYS):
    """最近底部最低价 → (最低价, 日期)：回看 scan_days 交易日（默认一年）内
    最低 low。hist 不含今日。

    不做左沿/近端 U 启发式——容差式左沿会被普通日内振幅误触发，把上升趋势中的
    短暂回调低点误判为底部（且结果随容差漂移）。一年最低点即谷底，确定唯一。
    """
    window = hist[-scan_days:] if len(hist) > scan_days else hist
    lo, ts = min((float(r[3]), r[0]) for r in window)
    return lo, ts


def screen_stock(code, kline_rows, name, today_str, max_rise):
    """
    对单只股票应用筛选条件。kline_rows 按 ts 升序，最后一行须为今日。

    Returns: (passed: bool, details: dict 或 {"reason": ...})
    """
    if len(kline_rows) < 3:
        return False, {"reason": "数据不足"}

    last = kline_rows[-1]
    if last[0].strftime("%Y-%m-%d") != today_str:
        return False, {"reason": "无今日bar"}

    today_open = float(last[1])
    current = float(last[4])

    yday = kline_rows[-2]
    yday_close = float(yday[4])
    prev_close = float(kline_rows[-3][4])

    market, num = code[:2], code[2:]

    # 1. 昨日涨停
    hit, limit = is_limit_up(yday_close, prev_close, market, num, name)
    if not hit:
        return False, {"reason": "昨日未涨停"}

    # 2. 今日低开
    if today_open >= yday_close:
        return False, {"reason": "未低开"}

    # 3. 距底部涨幅（底部 = 一年内最低 low，截至昨日）。max_rise=None (--y) 不设限，仅报告
    bottom_low, bottom_ts = find_recent_bottom(kline_rows[:-1])
    if bottom_low <= 0:
        return False, {"reason": "底部低点异常"}
    rise = (current - bottom_low) / bottom_low
    if max_rise is not None and rise > max_rise:
        return False, {"reason": "距底涨幅超标"}

    return True, {
        "yday_close": yday_close,
        "ratio": limit_ratio(market, num, name),
        "today_open": today_open,
        "open_drop": (yday_close - today_open) / yday_close,
        "current": current,
        "bottom_date": bottom_ts.strftime("%Y-%m-%d"),
        "bottom_low": bottom_low,
        "rise_from_bottom": rise,
    }


# ======================== 自检 ========================
def self_test():
    assert limit_up_price(9.95, 0.10) == 10.95, "四舍五入到分（浮点会得 10.94）"
    assert limit_up_price(3.55, 0.10) == 3.91, "half-up 而非银行家舍入"
    assert limit_ratio("sh", "600000", "浦发银行") == 0.10
    assert limit_ratio("sh", "600185", "*ST某某") == 0.05
    assert limit_ratio("sz", "300750", "宁德时代") == 0.20
    assert limit_ratio("sh", "688981", "中芯国际") == 0.20
    assert limit_ratio("bj", "430047", "诺思兰德") == 0.30

    def mk(day, o, h, l, c):
        return (datetime(2026, 8, day), o, h, l, c, 1000.0, "sh600000")

    # 底部 9.90 → 昨日前收 9.95 → 昨日涨停 10.95 → 今日低开 10.50 现 10.80
    rows = [mk(d, 10.2, 10.3, 10.0, 10.1) for d in range(1, 9)]
    rows.append(mk(10, 9.98, 10.05, 9.90, 9.95))   # 前收 9.95
    rows.append(mk(11, 10.00, 10.95, 9.98, 10.95))  # 昨日涨停
    rows.append(mk(12, 10.50, 10.90, 10.40, 10.80))  # 今日低开
    ok, d = screen_stock("sh600000", rows, "测试", "2026-08-12", 0.10)
    assert ok and abs(d["rise_from_bottom"] - (10.80 - 9.90) / 9.90) < 1e-9, d

    # 距底涨幅超标（现价 12.00 距底 9.90 涨 21%）
    rows[-1] = mk(12, 10.50, 12.10, 10.40, 12.00)
    ok, d = screen_stock("sh600000", rows, "测试", "2026-08-12", 0.10)
    assert not ok and d["reason"] == "距底涨幅超标", d

    # 未低开
    rows[-1] = mk(12, 11.00, 12.10, 10.90, 12.00)
    ok, d = screen_stock("sh600000", rows, "测试", "2026-08-12", 0.10)
    assert not ok and d["reason"] == "未低开", d

    # 昨日未涨停
    rows[-2] = mk(11, 10.00, 10.80, 9.98, 10.80)
    rows[-1] = mk(12, 10.50, 10.90, 10.40, 10.80)
    ok, d = screen_stock("sh600000", rows, "测试", "2026-08-12", 0.10)
    assert not ok and d["reason"] == "昨日未涨停", d

    # 底部=一年最低: 上升趋势中的短暂回调低点(9.5)不是底, 更深历史底(8.0)才是
    # （鸿博股份 2026-08 实例：7月深底 8.56 被误报为 08-14 回调低点 12.29 的回归）
    rows = [mk(1, 11.0, 11.5, 10.8, 11.2), mk(2, 11.0, 11.5, 10.8, 11.2),
            mk(3, 10.5, 10.6, 8.0, 8.5),          # 历史深底 8.0
            mk(4, 10.0, 11.0, 10.2, 10.8), mk(5, 10.2, 11.0, 10.2, 10.8),
            mk(6, 10.0, 10.1, 9.5, 9.7),          # 上升趋势中的回调低点（非底）
            mk(7, 9.8, 10.67, 9.6, 10.67),        # 昨日涨停: 9.7×1.1=10.67
            mk(8, 10.20, 10.5, 10.1, 10.30)]      # 今日低开
    ok, d = screen_stock("sh600000", rows, "测试", "2026-08-08", None)
    assert ok and abs(d["bottom_low"] - 8.0) < 1e-9 and d["bottom_date"] == "2026-08-03", d
    ok, d = screen_stock("sh600000", rows, "测试", "2026-08-08", 0.10)
    assert not ok and d["reason"] == "距底涨幅超标", d  # (10.30-8.0)/8.0 = 28.8%

    # 底部=一年最低: 阴跌后涨停, 谷底 9.0 (两日等低取最早日)
    rows = [mk(1, 10.0, 10.2, 10.0, 10.0), mk(2, 9.8, 9.9, 9.8, 9.8),
            mk(3, 9.1, 9.2, 9.0, 9.0),            # 谷底 9.0
            mk(4, 9.1, 9.9, 9.0, 9.9),            # 涨停: 9.0×1.1=9.9
            mk(5, 9.50, 9.8, 9.4, 9.60)]          # 今日低开
    ok, d = screen_stock("sh600000", rows, "测试", "2026-08-05", 0.10)
    assert ok and abs(d["bottom_low"] - 9.0) < 1e-9 and d["bottom_date"] == "2026-08-03", d

    # qfq: 08-09 除息 10派5（前收 10.0 → ef=0.95），除权前 low 10.0 复权到 9.5
    raw = [mk(d, 10.2, 10.3, 10.0, 10.1) for d in range(1, 8)]
    raw.append(mk(8, 10.0, 10.1, 10.0, 10.0))    # 事件前最后 bar, close 10.0
    raw.append(mk(9, 9.6, 9.7, 9.55, 9.45))      # 除权日
    raw.append(mk(10, 9.6, 10.40, 9.6, 10.40))   # 涨停: 9.45×1.1=10.395→10.40
    raw.append(mk(11, 10.00, 10.5, 9.9, 10.20))  # 今日低开 10.00 < 10.40
    ev = [("2026-08-09", 5.0, 0.0, 0.0, 0.0, 1, "除权除息")]
    adj = qfq_rows(raw, ev)
    assert qfq_rows(raw, None) is raw, "无事件原样返回"
    assert abs(adj[0][3] - 9.5) < 1e-9, f"除权前 low 应复权到 9.5: {adj[0][3]}"
    ok, d = screen_stock("sh600000", adj, "测试", "2026-08-11", 0.10)
    assert ok and abs(d["bottom_low"] - 9.5) < 1e-9, d  # 底部来自复权后除权前低点
    assert abs(d["rise_from_bottom"] - (10.20 - 9.5) / 9.5) < 1e-9, d

    # --y 模式: 不限距底涨幅（原 21% 超标用例改判通过，仍报告底部）
    rows = [mk(d, 10.2, 10.3, 10.0, 10.1) for d in range(1, 9)]
    rows.append(mk(10, 9.98, 10.05, 9.90, 9.95))
    rows.append(mk(11, 10.00, 10.95, 9.98, 10.95))
    rows.append(mk(12, 10.50, 12.10, 10.40, 12.00))
    ok, d = screen_stock("sh600000", rows, "测试", "2026-08-12", None)
    assert ok and abs(d["rise_from_bottom"] - (12.00 - 9.90) / 9.90) < 1e-9, d

    print("self-test OK")


# ======================== 主函数 ========================
def main():
    import argparse
    parser = argparse.ArgumentParser(description="昨日涨停今低开 — 底部区域回踩筛选")
    parser.add_argument("--all", action="store_true", help="筛全部 A 股 (默认仅自选股 zxg.blk)")
    parser.add_argument("--rise", type=float, default=MAX_RISE_FROM_BOTTOM,
                        help="距底部低点最大涨幅 (默认 0.10)")
    parser.add_argument("--y", action="store_true",
                        help="仅昨日涨停+今日低开，不限距底涨幅（仍检测并报告 U 型底）")
    parser.add_argument("--top", type=int, default=TOP_N, help="输出前 N 个结果")
    parser.add_argument("--json", action="store_true", help="JSON 输出 (stdout, 不写 xlsx)")
    parser.add_argument("--output-dir", default=os.path.join(OUTPUT_DIR, "find-retrace"), help="Excel 输出目录")
    parser.add_argument("--self-test", action="store_true", help="运行内置自检后退出")
    args = parser.parse_args()

    if args.self_test:
        self_test()
        return 0

    # 连接数据库
    url = f"taosws://{TDENGINE_USER}:{TDENGINE_PASS}@{TDENGINE_HOST}:{TDENGINE_PORT}/{TDENGINE_DB}"
    conn = taosws.connect(url)
    cursor = conn.cursor()
    names_by_code = load_stock_names(conn)

    t0 = time.time()
    today_str = time.strftime("%Y-%m-%d")

    db_codes = set(get_a_stock_codes(cursor))
    t1 = time.time()

    # 标的池
    if args.all:
        pool = set(all_mainboard_codes(conn))
        pool_desc = f"全 A 股(stock_name) {len(pool)} 只"
    else:
        pool = {parse_code(c) for c in zxg_codes()}
        pool_desc = f"自选股 zxg.blk {len(pool)} 只"

    a_stocks = sorted(db_codes & pool)
    print(f"[1/4] 标的池: {pool_desc} ∩ DB有数据 = {len(a_stocks)} 只 (耗时 {t1-t0:.2f}s)",
          file=sys.stderr)
    if not a_stocks:
        sys.stderr.write("无候选标的（自选股为空或无数据，加 --all 筛全市场）\n")
        return 1

    # 前复权事件（一次性批量拉取；无事件的股票 qfq_rows 直接跳过）
    adj_by_mc = batch_fetch_adjust(conn, a_stocks)
    print(f"[1/4] 复权事件: {len(adj_by_mc)} 只有除权记录", file=sys.stderr)

    start_date = (datetime.now() - timedelta(days=BOTTOM_SCAN_DAYS * 3 // 2 + 10)).strftime("%Y-%m-%d")

    all_results = []
    reject_count = Counter()
    total_batches = (len(a_stocks) + BATCH_SIZE - 1) // BATCH_SIZE

    for batch_idx in range(total_batches):
        batch_start = batch_idx * BATCH_SIZE
        batch = a_stocks[batch_start:batch_start + BATCH_SIZE]

        t2 = time.time()
        kline_rows = batch_query_kline(cursor, batch, start_date)
        t3 = time.time()

        stock_data = defaultdict(list)
        for row in kline_rows:
            stock_data[row[6]].append(row)

        batch_passed = 0
        for market, num in batch:
            code = f"{market}{num}"
            rows = stock_data.get(code, [])
            if not rows:
                reject_count["无数据"] += 1
                continue
            name = names_by_code.get(code, "")
            rows = qfq_rows(rows, adj_by_mc.get((market, num)))
            passed, details = screen_stock(code, rows, name, today_str,
                                           None if args.y else args.rise)
            if passed:
                batch_passed += 1
                details["name"] = name
                all_results.append((code, details))
            else:
                reject_count[details["reason"]] += 1

        print(f"[2/4] 批次 {batch_idx+1}/{total_batches}: {len(batch)} 只, "
              f"通过 {batch_passed}, kline {t3-t2:.1f}s", file=sys.stderr)

    conn.close()

    # 排序：距底涨幅升序（越贴近底部越靠前）
    all_results.sort(key=lambda x: x[1]["rise_from_bottom"])

    t5 = time.time()
    print(f"[3/4] 筛选完成: {len(all_results)} 只通过 (总耗时 {t5-t0:.2f}s)", file=sys.stderr)
    for reason, cnt in reject_count.most_common():
        print(f"       淘汰 {reason}: {cnt}", file=sys.stderr)
    if reject_count.get("无今日bar"):
        print("       提示: 存在无今日 bar 的标的，需先运行 fetch-today/fetch-kline 使当日数据入库",
              file=sys.stderr)

    mode_desc = "全 A 股" if args.all else "自选股"
    rise_desc = "距底涨幅不限(--y)" if args.y else \
        f"现价距一年内最低点涨幅 <= {args.rise:.0%}"
    if args.json:
        output = [{"code": code, **d} for code, d in all_results[:args.top]]
        print(json.dumps(output, ensure_ascii=False, indent=2, default=str))
    else:
        print(f"\n{'='*100}")
        print(f" 昨日涨停今低开筛选结果 [{mode_desc}] {today_str}")
        print(f" 条件: 昨日涨停, 今开<昨收, {rise_desc}")
        print(f" 共筛选 {len(a_stocks)} 只{mode_desc}, {len(all_results)} 只通过")
        print(f"{'='*100}")
        print(f"{'代码':<10} {'名称':<10} {'昨收':>8} {'涨停':>4} {'今开':>8} "
              f"{'低开':>6} {'现价':>8} {'底部日期':<12} {'底低':>8} {'距底':>6}")
        print(f"{'-'*100}")

        for code, d in all_results[:args.top]:
            print(
                f"{code:<10} {d['name']:<10} {d['yday_close']:>8.2f} "
                f"{d['ratio']:>4.0%} {d['today_open']:>8.2f} "
                f"{d['open_drop']:>5.1%} {d['current']:>8.2f} "
                f"{d['bottom_date']:<12} {d['bottom_low']:>8.2f} "
                f"{d['rise_from_bottom']:>5.1%}"
            )

        print(f"{'='*100}")
        print(f"[4/4] 输出前 {min(args.top, len(all_results))} 个结果", file=sys.stderr)

    if not args.json:
        stamped, n = write_xlsx(all_results, args.output_dir)
        print(f"[xlsx] → {stamped} (共 {n} 条)", file=sys.stderr)

    return 0


if __name__ == "__main__":
    sys.exit(main())
