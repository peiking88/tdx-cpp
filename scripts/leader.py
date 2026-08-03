#!/usr/bin/env python3
"""板块龙头选股 — 基于技术指标漏斗模型。

参考: "通过技术指标及盘口特征发现板块龙头股"。

四层漏斗:
  1. 板块层: 板块指数动量 (用板块指数涨幅代理, 无资金流数据)
  2. 技术初筛: RPS>80 + MA 趋势 + 52 周新高 + RSI/ADX/CCI + 口袋支点
  3. 盘口代理: 5 分钟量价行为 (放量确认, 无 Level-2 tick)
  4. 风控过滤: 长上影线假突破排除 + ATR 止损位

未实现 (需 Level-2 tick / 资金流数据):
  - 订单簿力量差异 (VDF/PDF)
  - 主动成交方向 (Delta 值)
  - 打板承接信号
  - RRG 象限 / VMCM 因子
"""

import argparse
import os
import sys
import time
from concurrent.futures import ThreadPoolExecutor, as_completed
import numpy as np
import pandas as pd
import taosws
from common import (ZXG_PATH, all_mainboard_codes, parse_code, zxg_codes,
                    apply_qfq, batch_fetch_adjust)

TDENGINE_URL = os.environ.get("TDENGINE_URL", "taosws://root:taosdata@localhost:6041")

# 板块指数 → 板块映射 (用于判断股票所属板块强度)
SECTOR_INDICES = {
    "sh000688": "科创",
    "sz399006": "创业板",
    "sz399001": "深证成指",
    "sh000001": "上证",
    "sh000300": "沪深300",
    "sh000905": "中证500",
    "sh000852": "中证1000",
    "bj899050": "北证50",
}

# 代码前缀 → 板块指数 (用于判断股票属于哪个板块)
CODE_SECTOR_MAP = [
    (("68",), "sh000688"),            # 科创板
    (("30",), "sz399006"),            # 创业板
    (("00", "60"), "sh000300"),       # 主板 (沪深300 代理)
    (("43", "83", "87", "92"), "bj899050"),  # 北交所 (含 920)
]


def connect():
    return taosws.connect(TDENGINE_URL)


def fetch_kline(conn, market, code, cycle="1d", days=400, min_rows=2):
    """单标的 K 线 (板块指数用)。个股批量见 batch_fetch_klines。"""
    tbl = f"tdx.k_{market}{code}_{cycle}"
    try:
        r = conn.query(
            f"SELECT ts, open, high, low, close, volume, amount "
            f"FROM {tbl} WHERE ts > NOW() - {days}d ORDER BY ts"
        )
    except Exception:
        return None
    rows = list(r)
    if not rows:
        return None
    df = pd.DataFrame(rows, columns=["ts", "O", "H", "L", "C", "V", "amount"])
    df["ts"] = pd.to_datetime(df["ts"])
    for c in ("O", "H", "L", "C", "V", "amount"):
        df[c] = pd.to_numeric(df[c], errors="coerce")
    df = df.dropna().reset_index(drop=True)
    return df if len(df) >= min_rows else None


def batch_fetch_klines(conn, pool, days=400):
    """批量查全市场 1d → {(market, code): DataFrame}。一次查询替代 N 次逐股 fetch_kline。

    按 (market, code) 聚合: tdx.kline 超级表虽有 market tag, 但同 code 不同市场是独立
    子表 (如 k_sh000027 / k_sz000027); 若按 code 聚合会丢失 market 维度导致混合。
    只保留 >=252 行的标的 (score_stock 需 250 周期指标)。
    """
    try:
        r = conn.query(
            f"SELECT market, code, ts, open, high, low, close, volume, amount "
            f"FROM tdx.kline WHERE cycle='1d' AND ts > NOW() - {days}d"
        )
    except Exception as e:
        sys.stderr.write(f"[error] 批量查询日线失败: {e}\n")
        return {}
    want = {(m, c) for m, c in pool}
    by_mc = {}
    for m, code, ts, o, h, l, c, v, amt in r:
        if (m, code) not in want:
            continue
        by_mc.setdefault((m, code), []).append((ts, o, h, l, c, v, amt))
    out = {}
    for mc, rows in by_mc.items():
        if len(rows) < 252:
            continue
        df = pd.DataFrame(rows, columns=["ts", "O", "H", "L", "C", "V", "amount"])
        df["ts"] = pd.to_datetime(df["ts"])
        for col in ("O", "H", "L", "C", "V", "amount"):
            df[col] = pd.to_numeric(df[col], errors="coerce")
        df = df.dropna().reset_index(drop=True)
        if len(df) >= 252:
            out[mc] = df
    return out


def load_stock_names(conn):
    """加载 {(market, code): name} 对照表。"""
    names = {}
    try:
        r = conn.query("SELECT market, code, name FROM tdx.stock_name")
        for m, c, n in r:
            names[(m, c)] = n
    except Exception:
        pass
    return names


# ---------------------------------------------------------------------------
# 技术指标计算
# ---------------------------------------------------------------------------
def add_indicators(df):
    """计算所有技术指标, 追加列到 df."""
    df = df.copy()

    # 均线
    df["MA5"] = df["C"].rolling(5).mean()
    df["MA20"] = df["C"].rolling(20).mean()
    df["MA60"] = df["C"].rolling(60).mean()
    df["MA120"] = df["C"].rolling(120).mean()
    df["MA250"] = df["C"].rolling(250).mean()

    # RPS: 250 日涨幅排名 (这里先算 250 日收益率, 后续在全市场排名)
    df["ret_250"] = df["C"].pct_change(250) * 100
    df["ret_120"] = df["C"].pct_change(120) * 100

    # 52 周新高 (250 日最高)
    df["high_250"] = df["C"].rolling(250).max()
    df["near_high"] = df["C"] / df["high_250"]  # 越接近 1 越接近新高

    # RSI (14)
    delta = df["C"].diff()
    gain = delta.clip(lower=0).rolling(14).mean()
    loss = (-delta.clip(upper=0)).rolling(14).mean()
    rs = gain / loss.replace(0, np.nan)
    df["RSI14"] = 100 - 100 / (1 + rs)

    # 趋势化 RSI: 最近一次超买(>70)与超卖(<30)谁更近
    # 简化: RSI > 50 视为多头趋势
    df["RSI_bull"] = (df["RSI14"] > 50).astype(int)

    # ADX / +DI / -DI
    df = add_dmi(df, period=14)

    # CCI (20)
    tp = (df["H"] + df["L"] + df["C"]) / 3
    tp_ma = tp.rolling(20).mean()
    tp_md = tp.rolling(20).apply(lambda x: np.mean(np.abs(x - np.mean(x))), raw=True)
    df["CCI"] = (tp - tp_ma) / (0.015 * tp_md)

    # ATR (14)
    tr = pd.concat([
        df["H"] - df["L"],
        (df["H"] - df["C"].shift()).abs(),
        (df["L"] - df["C"].shift()).abs()
    ], axis=1).max(axis=1)
    df["ATR14"] = tr.rolling(14).mean()
    df["ATR_pct"] = df["ATR14"] / df["C"] * 100

    # 口袋支点: 昨日量 > 10 日均量 1 倍且涨幅 > 7%
    vol_avg10 = df["V"].rolling(10).mean()
    df["vol_spike"] = df["V"] / vol_avg10
    df["pct_chg"] = df["C"].pct_change() * 100
    df["pocket_pivot"] = (df["vol_spike"] > 2.0) & (df["pct_chg"] > 7)

    # 量价确认: 上涨日放量
    df["vol_up_confirm"] = (df["pct_chg"] > 0) & (df["V"] > df["V"].shift(1) * 1.2)

    # 长上影线检测 (假突破过滤)
    upper_shadow = df["H"] - df[["O", "C"]].max(axis=1)
    body = (df["C"] - df["O"]).abs()
    body = body.replace(0, 0.001)
    df["upper_shadow_ratio"] = upper_shadow / body
    df["long_upper"] = df["upper_shadow_ratio"] > 2.0  # 上影 > 实体 2 倍

    return df


def add_dmi(df, period=14):
    """计算 ADX, +DI, -DI."""
    up = df["H"].diff()
    down = -df["L"].diff()
    plus_dm = np.where((up > down) & (up > 0), up, 0.0)
    minus_dm = np.where((down > up) & (down > 0), down, 0.0)
    tr = pd.concat([
        df["H"] - df["L"],
        (df["H"] - df["C"].shift()).abs(),
        (df["L"] - df["C"].shift()).abs()
    ], axis=1).max(axis=1)
    atr = tr.rolling(period).mean()
    plus_di = 100 * pd.Series(plus_dm, index=df.index).rolling(period).mean() / atr.replace(0, np.nan)
    minus_di = 100 * pd.Series(minus_dm, index=df.index).rolling(period).mean() / atr.replace(0, np.nan)
    dx = 100 * (plus_di - minus_di).abs() / (plus_di + minus_di).replace(0, np.nan)
    df["ADX"] = dx.rolling(period).mean()
    df["plus_DI"] = plus_di
    df["minus_DI"] = minus_di
    return df


def _compute(m, c, df, adj_events):
    """线程池入口: 前复权 → 算指标 → (m, c, df)。"""
    df = apply_qfq(df, adj_events)
    return m, c, add_indicators(df)


# ---------------------------------------------------------------------------
# 板块动量 (代理)
# ---------------------------------------------------------------------------
def sector_momentum(conn, days=20):
    """计算板块指数近 N 日涨幅, 返回 {sector_code: ret%}."""
    result = {}
    for code in SECTOR_INDICES:
        market = code[:2]
        symbol = code[2:]
        df = fetch_kline(conn, market, symbol, days=days + 5)
        if df is not None and len(df) >= 2:
            ret = (df["C"].iloc[-1] / df["C"].iloc[0] - 1) * 100
            result[code] = ret
    return result


def get_sector_for_code(code):
    """根据股票代码前缀 (前2位) 推断所属板块指数."""
    for prefixes, sector in CODE_SECTOR_MAP:
        if code[:2] in prefixes:
            return sector
    return "sh000300"  # 默认主板


# ---------------------------------------------------------------------------
# 评分
# ---------------------------------------------------------------------------
def hard_filter_fail(last, df):
    """六步硬过滤失败原因 (None=通过)。score_stock 与诊断共用, 避免阈值重复。"""
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
    """对单只股票评分, 返回 dict 或 None (不通过过滤)."""
    if df is None or len(df) < 252:   # ret_250=pct_change(250) 需 251 行
        return None

    last = df.iloc[-1]

    # ---- 硬过滤 (阈值集中在 hard_filter_fail, 诊断复用) ----
    if hard_filter_fail(last, df):
        return None
    ret_250 = last["ret_250"]

    # ---- 评分 (0-100) ----
    score = 0
    details = {}

    # RPS 动量 (30 分): ret_250 越高越好
    mom_score = min(ret_250 / 2, 30)  # 60% 涨幅封顶
    score += mom_score
    details["momentum"] = round(mom_score, 1)

    # 52 周新高距离 (20 分): near_high > 0.9 满分
    near_high = last.get("near_high", 0)
    high_score = max(0, min((near_high - 0.7) / 0.2 * 20, 20))
    score += high_score
    details["high_score"] = round(high_score, 1)

    # 均线多头排列 (15 分): MA5>MA20>MA60
    ma_score = 0
    if last["MA5"] > last["MA20"]:
        ma_score += 7
    if last["MA20"] > last.get("MA60", 0) and not pd.isna(last.get("MA60")):
        ma_score += 8
    score += ma_score
    details["ma_align"] = ma_score

    # RSI 强度 (10 分): 50-70 最佳
    rsi = last["RSI14"] if not pd.isna(last["RSI14"]) else 50
    rsi_score = max(0, 10 - abs(rsi - 60) / 5)
    score += rsi_score
    details["rsi"] = round(rsi_score, 1)

    # CCI 强度 (10 分): >100 满分
    cci = last["CCI"] if not pd.isna(last["CCI"]) else 0
    cci_score = min(max(cci / 20, 0), 10)
    score += cci_score
    details["cci"] = round(cci_score, 1)

    # 口袋支点 (10 分): 近 10 日出现过
    recent_10 = df.tail(10)
    pivot_score = 10 if recent_10["pocket_pivot"].any() else 0
    score += pivot_score
    details["pocket_pivot"] = pivot_score

    # 板块动量 (5 分): 所属板块近 20 日涨幅
    sec_score = max(0, min(sector_ret / 5, 5))
    score += sec_score
    details["sector"] = round(sec_score, 1)

    return {
        "score": round(score, 1),
        "price": round(last["C"], 2),
        "ret_250": round(ret_250, 1),
        "near_high": round(near_high, 3),
        "RSI14": round(rsi, 1),
        "ADX": round(last["ADX"], 1),
        "CCI": round(cci, 1),
        "ATR_pct": round(last["ATR_pct"], 2) if not pd.isna(last["ATR_pct"]) else None,
        "MA5": round(last["MA5"], 2),
        "MA20": round(last["MA20"], 2),
        **details,
    }


# ---------------------------------------------------------------------------
# 主流程
# ---------------------------------------------------------------------------
def main():
    ap = argparse.ArgumentParser(description="板块龙头选股 (技术指标漏斗)")
    ap.add_argument("--codes", nargs="*", help="指定代码")
    ap.add_argument("--zxg", action="store_true", help="自选股")
    ap.add_argument("--all", action="store_true", help="全主板")
    ap.add_argument("--top", type=int, default=20, help="输出 top N")
    ap.add_argument("--min-score", type=float, default=40, help="最低评分")
    ap.add_argument("--rps", type=float, default=80, help="RPS 最低分位 (默认 80)")
    ap.add_argument("--diagnostic", action="store_true",
                    help="显示 RPS 通过但其他过滤未通过的股票 (诊断模式)")
    ap.add_argument("--limit", type=int, help="最多分析多少只 (调试)")
    ap.add_argument("--output-dir", default="output/leader", help="输出目录")
    ap.add_argument("--workers", type=int, default=8, help="并发线程数 (默认 8)")
    args = ap.parse_args()

    conn = connect()

    # 标的池
    if args.codes:
        pool = [parse_code(c) for c in args.codes]
    elif args.zxg:
        pool = [parse_code(c) for c in zxg_codes()]
    elif args.all:
        pool = all_mainboard_codes(conn)
    else:
        pool = [parse_code(c) for c in zxg_codes()]
    if not pool:
        print("[error] empty pool", file=sys.stderr)
        sys.exit(1)
    if args.limit:
        pool = pool[: args.limit]

    print(f"[pool] {len(pool)} stocks")

    # 股票名称对照
    names = load_stock_names(conn)

    # 板块动量
    print("[sector] computing momentum...")
    sec_mom = sector_momentum(conn)
    for code, ret in sorted(sec_mom.items(), key=lambda x: -x[1]):
        print(f"  {SECTOR_INDICES.get(code, code):8s} {code}: {ret:+.2f}% (20d)")

    # 批量拉日线 (一次查询替代 N 次) + 批量复权事件
    print(f"[fetch] 批量拉取 {len(pool)} 只日线...")
    klines = batch_fetch_klines(conn, pool, days=400)
    print(f"[fetch] {len(klines)} 只有足够数据 (>=252 行)")
    adj_by_mc = batch_fetch_adjust(conn, pool)
    print(f"[fetch] {len(adj_by_mc)} 只有除权事件 (应用前复权)")

    all_features = []
    with ThreadPoolExecutor(max_workers=args.workers) as ex:
        futs = [ex.submit(_compute, m, c, klines[(m, c)], adj_by_mc.get((m, c)))
                for m, c in pool if (m, c) in klines]
        for f in as_completed(futs):
            try:
                all_features.append(f.result())
            except Exception as e:
                sys.stderr.write(f"[warn] 指标计算异常: {e}\n")
    print(f"[fetch] {len(all_features)} 只完成指标计算")
    if not all_features:
        print("[error] no data", file=sys.stderr)
        sys.exit(1)

    # 全市场 RPS 排名 (ret_250 的 percentile)
    all_ret = []
    for m, c, df in all_features:
        if len(df) > 1 and not pd.isna(df.iloc[-1].get("ret_250")):
            all_ret.append((m, c, df.iloc[-1]["ret_250"]))
    if not all_ret:
        print("[error] no valid ret_250", file=sys.stderr)
        sys.exit(1)

    rets = np.array([x[2] for x in all_ret])

    # 评分
    results = []
    diagnostic = []
    for m, c, df in all_features:
        last_ret = df.iloc[-1]["ret_250"] if not pd.isna(df.iloc[-1].get("ret_250")) else None
        if last_ret is None:
            continue
        # RPS 排名: last_ret 在样本(上市满250天的子集)内的分位; < 不含自身
        rps = (rets < last_ret).mean() * 100
        if rps < args.rps:
            continue

        sector = get_sector_for_code(c)
        sec_ret = sec_mom.get(sector, 0)

        result = score_stock(df, sec_ret)
        if result is None:
            if args.diagnostic:
                last = df.iloc[-1]
                diagnostic.append({
                    "market": m, "code": c, "RPS": round(rps, 1),
                    "price": round(last["C"], 2),
                    "ret_250": round(last["ret_250"], 1) if not pd.isna(last["ret_250"]) else 0,
                    "reason": hard_filter_fail(last, df) or "filtered",
                })
            continue
        result["market"] = m
        result["code"] = c
        result["name"] = names.get((m, c), "")
        result["RPS"] = round(rps, 1)
        result["sector"] = SECTOR_INDICES.get(sector, sector)
        results.append(result)

    # 排序
    results.sort(key=lambda x: -x["score"])

    # 输出
    os.makedirs(args.output_dir, exist_ok=True)
    df_out = pd.DataFrame(results)
    if not df_out.empty:
        df_out["stop_loss"] = df_out["price"] * (1 - df_out["ATR_pct"].fillna(0) / 100 * 2)
        cols = ["market", "code", "name", "score", "RPS", "price", "ret_250", "near_high",
                "RSI14", "ADX", "CCI", "ATR_pct", "stop_loss", "sector",
                "momentum", "high_score", "ma_align", "rsi", "cci", "pocket_pivot"]
        df_out = df_out[[c for c in cols if c in df_out.columns]]
        # 日期戳 CSV (浮点保留3位)
        csv_file = os.path.join(args.output_dir, f"leader-{time.strftime('%Y%m%d')}.csv")
        df_out.round(3).to_csv(csv_file, index=False)
        # 兼容旧名
        df_out.to_csv(os.path.join(args.output_dir, "leader.csv"), index=False)

    print(f"\n=== 板块龙头候选 (top {args.top}, score >= {args.min_score}) ===")
    filtered = [r for r in results if r["score"] >= args.min_score]
    if not filtered:
        print("  (无通过过滤的股票)")
    else:
        print(f"{'排名':>4} {'代码':<10} {'名称':<8} {'评分':>6} {'RPS':>6} {'价格':>8} {'250日%':>8} "
              f"{'近新高':>6} {'RSI':>5} {'ADX':>5} {'CCI':>6} {'ATR%':>5} {'止损位':>8} {'板块'}")
        for i, r in enumerate(filtered[:args.top], 1):
            nm = r.get('name', '') or ''
            atr = r.get("ATR_pct", 0) or 0
            stop = r["price"] * (1 - atr / 100 * 2)
            print(f"{i:4d} {r['market']}{r['code']:<8} {nm:<8} {r['score']:6.1f} {r['RPS']:6.1f} "
                  f"{r['price']:8.2f} {r['ret_250']:8.1f} {r['near_high']:6.3f} "
                  f"{r['RSI14']:5.1f} {r['ADX']:5.1f} {r['CCI']:6.1f} "
                  f"{atr:5.2f} {stop:8.2f} {r.get('sector', '')}")

    # 诊断输出
    if args.diagnostic and diagnostic:
        diagnostic.sort(key=lambda x: -x["RPS"])
        print(f"\n=== 诊断: RPS>={args.rps} 但未通过过滤 (top 15) ===")
        print(f"{'代码':<10} {'RPS':>6} {'价格':>8} {'250日%':>8} {'过滤原因'}")
        for d in diagnostic[:15]:
            print(f"{d['market']}{d['code']:<8} {d['RPS']:6.1f} {d['price']:8.2f} "
                  f"{d['ret_250']:8.1f} {d['reason']}")

    # 保存到通达信自选板块 (目录与 ZXG_PATH 同源)
    blk_dir = os.path.dirname(ZXG_PATH)
    os.makedirs(blk_dir, exist_ok=True)
    written = 0
    with open(os.path.join(blk_dir, "LT.blk"), "w", newline="") as f:
        f.write("1999999\r\n")
        for r in filtered[:args.top]:
            if r["market"] == "bj":
                continue  # blk 格式仅 1=sh/0=sz 两位前缀, 无法表示北交所
            prefix = "1" if r["market"] == "sh" else "0"
            f.write(f"{prefix}{r['code']}\r\n")
            written += 1
    print(f"[blk] → LT.blk ({written} 只)")

    print(f"\n[output] → {args.output_dir}/leader.csv")
    print(f"[summary] {len(results)} passed filters, {len(filtered)} with score >= {args.min_score}")


if __name__ == "__main__":
    main()
