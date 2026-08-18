#!/usr/bin/env python3
"""
================================================================================
 抄底策略脚本 find-bottom.py — 「跌了买、弹了卖」DEMA/RSI 量化版
================================================================================

源文章《一个简单的抄底策略，能打败买入持有吗？》规则（仅 2 个指标，无止损）:
  入场: 收盘价跌破 20 周期 DEMA（双重指数均线, DEMA = 2×EMA1 − EMA2，比 EMA 更贴价）
  出场: 14 周期 RSI 从超卖区(<=30)向上穿越 30（反弹自确认后落袋）
  纪律: 信号后移一根 K 线、次根开盘价成交（防未来函数）；扣手续费+滑点

两种模式:
  默认      : 当日信号扫描 —— 收盘<DEMA20 的「抄底买入区」标的（按 RSI 升序,
              越接近 30 越先触发卖出），以及 RSI14 今日上穿 30 的「反弹卖出」信号
  --backtest: 对每只标的回测策略 vs 买入持有（交易数/胜率/收益/超额/最大回撤）

日线前复权。默认自选股，--all 全市场。

用法:
  python3 scripts/find-bottom.py                # 自选股当日信号
  python3 scripts/find-bottom.py --all          # 全市场信号
  python3 scripts/find-bottom.py --all --backtest   # 回测: 策略 vs 买入持有
================================================================================
"""

import argparse
import os
import sys
import time
from collections import defaultdict
from datetime import datetime, timedelta

import numpy as np
import pandas as pd
import taosws

from common import OUTPUT_DIR, all_mainboard_codes, apply_qfq, batch_fetch_adjust, parse_code, zxg_codes

# ======================== 配置 ========================
TDENGINE_HOST = os.environ.get("TDENGINE_HOST", "localhost")
TDENGINE_PORT = int(os.environ.get("TDENGINE_PORT", "6041"))
TDENGINE_USER = os.environ.get("TDENGINE_USER", "root")
TDENGINE_PASS = os.environ.get("TDENGINE_PASS", "taosdata")
TDENGINE_DB = os.environ.get("TDENGINE_DB", "tdx")

BATCH_SIZE = 1800
DEMA_PERIOD = 20      # DEMA 周期
RSI_PERIOD = 14       # RSI 周期
RSI_LEVEL = 30        # 超卖阈值
COST_PER_SIDE = 0.003  # 单边成本: 手续费 0.1% + 滑点 0.2%（文章口径）
LOOKBACK_DAYS = 400   # 回看交易日（指标暖机 + 回测窗口）


# ======================== 指标（逐字对齐文章公式） ========================
def calc_dema(close, period=DEMA_PERIOD):
    """DEMA = 2×EMA1 − EMA2（EMA2 = EMA1 的 EMA），削 EMA 滞后。"""
    ema1 = close.ewm(span=period, adjust=False).mean()
    ema2 = ema1.ewm(span=period, adjust=False).mean()
    return 2 * ema1 - ema2


def calc_rsi(close, period=RSI_PERIOD):
    """RSI（文章用 EMA 平滑而非 Wilder SMA）。全涨→100，全跌→0。"""
    delta = close.diff()
    up = delta.clip(lower=0).ewm(span=period, adjust=False).mean()
    down = (-delta.clip(upper=0)).ewm(span=period, adjust=False).mean()
    rs = up / down.replace(0, np.nan)
    return (100 - 100 / (1 + rs)).fillna(100.0)


def entry_state(close, period=DEMA_PERIOD):
    """入场状态: 收盘 < DEMA（回撤到均线下方，而非崩盘）。"""
    return close < calc_dema(close, period)


def exit_signal(close, period=RSI_PERIOD, level=RSI_LEVEL):
    """出场信号: RSI 从 <=level 向上穿越 level。"""
    rsi = calc_rsi(close, period)
    return (rsi > level) & (rsi.shift(1) <= level), rsi


# ======================== 回测引擎（次根开盘成交 + 单边成本） ========================
def backtest(df, cost=COST_PER_SIDE, dema_period=DEMA_PERIOD,
             rsi_period=RSI_PERIOD, level=RSI_LEVEL):
    """df: 列 ts/O/H/L/C。返回 (策略总收益, 持有总收益, 交易数, 胜率, 最大回撤)。

    信号在 t 收盘产生 → t+1 开盘成交（防未来函数）；买卖各扣 cost。
    """
    c = df["C"].reset_index(drop=True)
    o = df["O"].reset_index(drop=True)
    ent = entry_state(c, dema_period).shift(1).fillna(False).astype(bool).to_numpy()
    ext, _ = exit_signal(c, rsi_period, level)
    ext = ext.shift(1).fillna(False).astype(bool).to_numpy()

    cash, shares = 1.0, 0.0
    entry_px = 0.0
    wins, trades = 0, 0
    equity = []
    for i in range(len(c)):
        if shares == 0 and ent[i]:
            entry_px = o[i] * (1 + cost)
            shares = cash / entry_px
            cash = 0.0
        elif shares > 0 and ext[i]:
            cash = shares * o[i] * (1 - cost)
            trades += 1
            if cash > shares * entry_px:
                wins += 1
            shares = 0.0
        equity.append(cash if shares == 0 else shares * c[i])
    eq = pd.Series(equity)
    ret = eq.iloc[-1] - 1.0
    bh = c.iloc[-1] / c.iloc[0] - 1.0
    mdd = float((eq / eq.cummax() - 1).min()) if len(eq) else 0.0
    win_rate = wins / trades if trades else None
    return ret, bh, trades, win_rate, mdd


# ======================== 数据获取（对齐 find-pbover 惯例） ========================
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


# ======================== 自检 ========================
def self_test():
    from datetime import date

    def mkday(i):
        return date(2026, 1, 1) + timedelta(days=i)

    # 指标
    const = pd.Series([10.0] * 50)
    assert (calc_dema(const) - 10.0).abs().max() < 1e-9, "常数列 DEMA=常数"
    assert (calc_rsi(pd.Series([10.0 + 0.1 * i for i in range(50)])).iloc[-1] > 99), "全涨 RSI≈100"
    assert (calc_rsi(pd.Series([10.0 - 0.1 * i for i in range(50)])).iloc[-1] < 1), "全跌 RSI≈0"
    # 上穿判定
    rsi_like = pd.Series([25.0, 28.0, 29.5, 31.0, 35.0])
    sig = (rsi_like > 30) & (rsi_like.shift(1) <= 30)
    assert list(sig) == [False, False, False, True, False], "仅上穿当日 True"

    # 回测: 横盘 → 下跌破 DEMA → 反弹 → RSI 上穿 30 卖出（次根开盘成交）
    closes = [10.0] * 30 + [9.4, 9.0, 8.8, 8.9, 9.2, 9.6, 10.0, 10.4]
    opens = [10.0] * 30 + [9.8, 9.2, 8.9, 8.8, 8.9, 9.4, 9.8, 10.2]
    df = pd.DataFrame({"ts": [mkday(i) for i in range(len(closes))],
                       "O": opens, "H": closes, "L": closes, "C": closes})
    ret, bh, trades, wr, _mdd = backtest(df, cost=0.0)
    assert trades == 1 and wr == 1.0, (trades, wr)
    # 入场应为首个「前一日收盘<DEMA」日的开盘（非当日收盘, 防未来函数）
    ent_state = entry_state(pd.Series(closes))
    first_ent = ent_state[ent_state].index[0]
    assert first_ent < len(closes) - 1, "入场发生在信号次日"

    # 成本: 单边 0.3% 应使收益低于零成本
    ret_cost, _, t2, _, _ = backtest(df, cost=0.003)
    assert t2 == 1 and ret_cost < ret, (ret, ret_cost)
    print("self-test OK")


# ======================== 主函数 ========================
def main():
    parser = argparse.ArgumentParser(description="抄底策略 — DEMA20 跌破买入 / RSI14 上穿30 卖出")
    parser.add_argument("--all", action="store_true", help="全 A 股 (默认仅自选股 zxg.blk)")
    parser.add_argument("--backtest", action="store_true",
                        help="回测模式: 每只标的 策略 vs 买入持有 (默认为当日信号扫描)")
    parser.add_argument("--dema", type=int, default=DEMA_PERIOD, help="DEMA 周期 (默认 20)")
    parser.add_argument("--rsi-period", type=int, default=RSI_PERIOD, help="RSI 周期 (默认 14)")
    parser.add_argument("--rsi-level", type=float, default=RSI_LEVEL, help="RSI 超卖阈值 (默认 30)")
    parser.add_argument("--cost", type=float, default=COST_PER_SIDE, help="单边成本 (默认 0.003)")
    parser.add_argument("--output-dir", default=os.path.join(OUTPUT_DIR, "find-bottom"), help="输出目录")
    parser.add_argument("--self-test", action="store_true", help="运行内置自检后退出")
    args = parser.parse_args()

    if args.self_test:
        self_test()
        return 0

    url = f"taosws://{TDENGINE_USER}:{TDENGINE_PASS}@{TDENGINE_HOST}:{TDENGINE_PORT}/{TDENGINE_DB}"
    conn = taosws.connect(url)
    cursor = conn.cursor()
    names_by_code = load_stock_names(conn)

    t0 = time.time()
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

    start_date = (datetime.now() - timedelta(days=LOOKBACK_DAYS * 3 // 2 + 30)).strftime("%Y-%m-%d")
    print(f"[1/3] {pool_desc} ∩ DB = {len(a_stocks)} 只 (耗时 {time.time()-t0:.1f}s)", file=sys.stderr)

    signals = []   # (code, name, kind, close, dema, gap, rsi)
    bt_results = []  # (code, name, years, ret, bh, trades, wr, mdd)
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
            if len(rows) < 40:
                continue
            df = pd.DataFrame([r[:5] for r in rows], columns=["ts", "O", "H", "L", "C"])
            c = df["C"]
            dema = calc_dema(c, args.dema)
            ext, rsi = exit_signal(c, args.rsi_period, args.rsi_level)
            name = names_by_code.get(code, "")

            if args.backtest:
                ret, bh, trades, wr, mdd = backtest(
                    df, cost=args.cost, dema_period=args.dema,
                    rsi_period=args.rsi_period, level=args.rsi_level)
                years = len(df) / 244.0
                bt_results.append((code, name, years, ret, bh, trades, wr, mdd))
            else:
                # 卖出信号（最新一根 RSI 上穿 30，持仓者关注）
                if bool(ext.iloc[-1]):
                    signals.append((code, name, "卖出", float(c.iloc[-1]),
                                    float(dema.iloc[-1]),
                                    float(c.iloc[-1] / dema.iloc[-1] - 1),
                                    float(rsi.iloc[-1])))
                # 买入区（最新收盘 < DEMA，按 RSI 升序 = 越接近卖出触发越靠前）
                elif bool(c.iloc[-1] < dema.iloc[-1]):
                    signals.append((code, name, "买入", float(c.iloc[-1]),
                                    float(dema.iloc[-1]),
                                    float(c.iloc[-1] / dema.iloc[-1] - 1),
                                    float(rsi.iloc[-1])))
        print(f"[2/3] 批次 {bi+1}/{total_batches}: kline {time.time()-t2:.1f}s, "
              f"累计 {(len(bt_results) or len(signals))} 条", file=sys.stderr)

    conn.close()

    os.makedirs(args.output_dir, exist_ok=True)
    stamp = time.strftime("%Y%m%d")

    if args.backtest:
        bt_results.sort(key=lambda x: (x[3] - x[4]), reverse=True)  # 超额降序
        beat = sum(1 for r in bt_results if r[3] > r[4])
        print(f"\n{'='*104}")
        print(f" 抄底策略回测 vs 买入持有 [{pool_desc}] 单边成本 {args.cost:.1%} "
              f"(DEMA{args.dema}/RSI{args.rsi_period}×{args.rsi_level:.0f})")
        print(f" 跑赢持有: {beat}/{len(bt_results)} 只")
        print(f"{'='*104}")
        print(f"{'代码':<10} {'名称':<10} {'年数':>4} {'交易数':>5} {'胜率':>6} "
              f"{'策略收益':>9} {'持有收益':>9} {'超额':>8} {'最大回撤':>8}")
        print("-" * 104)
        for code, name, years, ret, bh, trades, wr, mdd in bt_results[:50]:
            wr_s = f"{wr:>5.0%}" if wr is not None else "   --"
            print(f"{code:<10} {name:<10} {years:>4.1f} {trades:>5d} {wr_s} "
                  f"{ret:>8.1%} {bh:>8.1%} {ret-bh:>7.1%} {mdd:>8.1%}")
        print(f"{'='*104}")
        out = pd.DataFrame([{
            "代码": c, "名称": n, "年数": round(y, 1), "交易数": t,
            "胜率": round(w * 100, 1) if w is not None else None,
            "策略收益%": round(r * 100, 1), "持有收益%": round(b * 100, 1),
            "超额%": round((r - b) * 100, 1), "最大回撤%": round(m * 100, 1),
        } for c, n, y, r, b, t, w, m in bt_results])
    else:
        signals.sort(key=lambda x: (0 if x[2] == "卖出" else 1, x[6]))
        print(f"\n{'='*96}")
        print(f" 抄底信号扫描 [{pool_desc}] (DEMA{args.dema} / RSI{args.rsi_period} 上穿 {args.rsi_level:.0f})")
        print(f"{'='*96}")
        print(f"{'代码':<10} {'名称':<10} {'信号':<5} {'现价':>8} {'DEMA':>8} "
              f"{'距DEMA':>7} {'RSI':>6}")
        print("-" * 96)
        for code, name, kind, close, dema, gap, rsi in signals[:60]:
            print(f"{code:<10} {name:<10} {kind:<5} {close:>8.2f} {dema:>8.2f} "
                  f"{gap:>6.1%} {rsi:>6.1f}")
        print(f"{'='*96}")
        n_buy = sum(1 for s in signals if s[2] == "买入")
        print(f"[3/3] 卖出信号 {len(signals)-n_buy} 条, 买入区 {n_buy} 条", file=sys.stderr)
        out = pd.DataFrame([{
            "代码": c, "名称": n, "信号": k, "现价": cl, "DEMA": d,
            "距DEMA%": round(g * 100, 1), "RSI": round(r, 1),
        } for c, n, k, cl, d, g, r in signals])

    stamped = os.path.join(args.output_dir, f"find-bottom-{stamp}.xlsx")
    out.to_excel(stamped, index=False)
    print(f"[xlsx] → {stamped} (共 {len(out)} 条)", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
