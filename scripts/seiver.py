#!/usr/bin/env python3
"""seiver (筛子) — 两套选股方法, --methods 参数化组合。

方法 (任选, 默认两法并用 OR 合并):
  leader (龙头漏斗): 四层技术指标漏斗 (RPS + MA + 新高 + RSI/ADX/CCI + 口袋支点 + 风控)
  dc    (二次穿越) : 周线 MACD DIFF 两次上穿 DEA, 中间夹死叉 (洗盘),
                     第二次金叉在零轴附近/上方 + 放量 + 站上周均线
  div   (分钟信号) : 30m MACD 底背离 (价 LL + DIFF HL) + 5m MACD 零轴上金叉

命中标记 strategy: 漏斗 / 二次穿越 / 双命中 (两法同中)。dc-only 时跳过日线指标计算。

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
import threading
import numpy as np
import pandas as pd
import taosws
from common import (ZXG_PATH, all_mainboard_codes, parse_code, zxg_codes,
                    apply_qfq, batch_fetch_adjust, pad)

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


def batch_fetch_klines(conn, pool, days=400, min_rows=252):
    """批量查全市场 1d → {(market, code): DataFrame}。一次查询替代 N 次逐股 fetch_kline。

    按 (market, code) 聚合: tdx.kline 超级表虽有 market tag, 但同 code 不同市场是独立
    子表 (如 k_sh000027 / k_sz000027); 若按 code 聚合会丢失 market 维度导致混合。
    只保留 >=min_rows 行的标的: leader 需 252 (ret_250=pct_change(250)); dc-only 需 ~180
    (周线二次穿越需 >=35 周)。
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
        if len(rows) < min_rows:
            continue
        df = pd.DataFrame(rows, columns=["ts", "O", "H", "L", "C", "V", "amount"])
        df["ts"] = pd.to_datetime(df["ts"])
        for col in ("O", "H", "L", "C", "V", "amount"):
            df[col] = pd.to_numeric(df[col], errors="coerce")
        df = df.dropna().reset_index(drop=True)
        if len(df) >= min_rows:
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


def _compute(m, c, df, adj_events, need_ind=True):
    """线程池入口: 前复权 → (可选) 算日线指标 → (m, c, df)。

    need_ind=False 时只复权不算指标 (dc-only 模式跳过 RSI/ADX/CCI 等省时)。
    """
    df = apply_qfq(df, adj_events)
    return m, c, (add_indicators(df) if need_ind else df)


# ---------------------------------------------------------------------------
# 周线 MACD 二次穿越
# ---------------------------------------------------------------------------
def to_weekly(df):
    """日线 (已前复权) → 周线, W-FRI 周五为周收盘 (对齐通达信周线)。

    必须在 qfq 之后重采样: TDengine 存的是未复权价, 除权日跳空会让周线 MACD 出现伪金叉。
    pandas W-FRI 等价于 TDengine INTERVAL(1w, 2d) 的周五周, 但作用在已复权价上。
    """
    return (df.set_index("ts").resample("W-FRI")
            .agg({"O": "first", "H": "max", "L": "min", "C": "last", "V": "sum", "amount": "sum"})
            .dropna(subset=["C"]))


def detect_double_cross(w, fresh=4, lookback=30, zero_band=0.02,
                        vol_ratio=1.3, min_gap=3):
    """周线 MACD 二次穿越信号。返回 dict (dc_score + 细节) 或 None。

    结构定义:
      回溯窗口内出现两次金叉 (g1=首次建仓, g2=二次穿越), 中间夹死叉 (洗盘);
      g2 为最近一次穿越 (其后无死叉, 当前处于金叉状态)、发生在 fresh 周内;
      g2 处 DEA 在零轴附近或上方 (dea_g2 >= -zero_band*价)。

    三项确认 (计入 dc_score, 非硬门槛):
      放量 — 近 3 周量逐增, 或当周量 > vol_ratio × 洗盘期均量;
      周均线多头 — 收盘价站上 MA5/10/20/60 周线 (各 5 分)。
    """
    if w is None or len(w) < 35:      # EMA26 + EMA9 收敛需足够周数
        return None
    c = w["C"].astype(float)
    v = w["V"].astype(float)
    diff = c.ewm(span=12, adjust=False).mean() - c.ewm(span=26, adjust=False).mean()
    dea = diff.ewm(span=9, adjust=False).mean()
    dv, ev, n = diff.values, dea.values, len(w)

    # 定位金叉 / 死叉 (按周)
    gc, dc_at = [], []
    for i in range(1, n):
        if dv[i - 1] <= ev[i - 1] and dv[i] > ev[i]:
            gc.append(i)
        elif dv[i - 1] >= ev[i - 1] and dv[i] < ev[i]:
            dc_at.append(i)
    gc_win = [i for i in gc if i >= n - lookback]
    if len(gc_win) < 2:               # 不足两次金叉
        return None
    g2, g1 = gc_win[-1], gc_win[-2]
    if g2 - g1 < min_gap:             # 洗盘期过短, g1/g2 可能是同一波噪声
        return None
    if dc_at and dc_at[-1] > g2:      # g2 后又死叉 → 信号已失效
        return None
    if g2 < n - fresh:                # g2 不够新鲜
        return None

    dea_g2 = float(dea.iloc[g2])
    price_g2 = float(c.iloc[g2])
    if dea_g2 < -zero_band * price_g2:     # 确认 (硬): 零轴附近/上方
        return None

    last3 = v.iloc[-3:]
    vol_rising = len(last3) >= 3 and bool(last3.is_monotonic_increasing)
    wash_v = v.iloc[g1:g2].mean()
    v_now = float(v.iloc[-1])
    vol_spike = bool(wash_v and wash_v > 0 and v_now > vol_ratio * wash_v)

    last_c = float(c.iloc[-1])
    ma_above = {p: (not pd.isna(m) and last_c > float(m))
                for p, m in ((p, c.rolling(p).mean().iloc[-1]) for p in (5, 10, 20, 60))}

    score = 50.0                                       # 结构分
    score += 15 if dea_g2 > 0 else 8                   # 零轴强度
    score += 15 if vol_rising else (8 if vol_spike else 0)
    score += 5 * sum(ma_above.values())                # 周均线多头 (满分 20)
    score = min(score, 100.0)

    return {
        "dc_score": round(score, 1),
        "dc_diff": round(float(diff.iloc[-1]), 3),
        "dc_dea": round(float(dea.iloc[-1]), 3),
        "dc_g2": w.index[g2].strftime("%Y-%m-%d"),
        "dc_age": n - 1 - g2,
        "dc_zero": "零轴上" if dea_g2 > 0 else "零轴附近",
        "dc_vol": "3周放量" if vol_rising else ("当周放量" if vol_spike else "量平"),
        "dc_ma": f"{sum(ma_above.values())}/4",
        "dc_ma_ok": all(ma_above.values()),
    }


# ---------------------------------------------------------------------------
# div: 30m MACD 底背离 + 5m MACD 零轴上金叉
# ---------------------------------------------------------------------------
_AM_START, _AM_END = 570, 690    # 9:30 ~ 11:30 (分钟)
_PM_START, _PM_END = 780, 900    # 13:00 ~ 15:00


def resample_intraday(df, tf):
    """5 分钟 → tf 分钟 (30/60/120), A 股时段感知: 上午/下午各自分桶, 不跨午休。

    TDengine 5m 用 bar 结束时间戳 (9:35 = [9:30,9:35)), 故先减 5min 得区间起点再按
    时段 (9:30/13:00) 切桶: 每交易日 30m=8 / 60m=4 / 120m=2 根。
    """
    ist = df["ts"] - pd.Timedelta(minutes=5)
    minutes = ist.dt.hour * 60 + ist.dt.minute
    am = (minutes >= _AM_START) & (minutes < _AM_END)
    pm = (minutes >= _PM_START) & (minutes < _PM_END)
    if not (am.any() or pm.any()):
        return None
    slot = np.where(am, (minutes - _AM_START) // tf, np.where(pm, (minutes - _PM_START) // tf, -1))
    sess = np.where(am, "AM", np.where(pm, "PM", "X"))
    d = df.assign(_d=ist.dt.date, _s=sess, _k=slot)
    d = d[d["_s"] != "X"]
    g = (d.groupby(["_d", "_s", "_k"])
         .agg(O=("O", "first"), H=("H", "max"), L=("L", "min"), C=("C", "last"),
              V=("V", "sum"), ts=("ts", "last"))
         .reset_index(drop=True))
    return g if len(g) else None


def _pivot_lows(cv, k=3, sep=4):
    """显著波谷: 严格局部极小 + ±k 窗口最小值; 相邻 <sep 的合并取最低 (去噪)。"""
    n = len(cv)
    cand = [i for i in range(k, n - k)
            if cv[i] < cv[i - 1] and cv[i] < cv[i + 1] and cv[i] <= cv[i - k:i + k + 1].min()]
    piv = []
    for i in cand:
        if piv and i - piv[-1] < sep:
            if cv[i] < cv[piv[-1]]:
                piv[-1] = i
        else:
            piv.append(i)
    return piv


def detect_bottom_div(bars, k=3, sep=4, fresh=8):
    """单周期 MACD 底背离: 最近两个波谷 p1<p2, 价 CV[p2]<CV[p1] (新低) 但 DIFF[p2]>DIFF[p1]
    (抬高) → 下跌动能衰竭。fresh 为 p2 距今最大 bar 数。返回细节 dict 或 None。"""
    if bars is None or len(bars) < 35:
        return None
    c = bars["C"].astype(float)
    diff = c.ewm(span=12, adjust=False).mean() - c.ewm(span=26, adjust=False).mean()
    cv, dv, n = c.values, diff.values, len(c)
    piv = _pivot_lows(cv, k, sep)
    if len(piv) < 2:
        return None
    p1, p2 = piv[-2], piv[-1]
    if not (cv[p2] < cv[p1] and dv[p2] > dv[p1]):   # 价跌 + DIFF 抬高 = 底背离
        return None
    if p2 < n - fresh:                               # 信号不够新鲜
        return None
    return {
        "p2_ts": str(bars["ts"].iloc[p2])[:16],
        "age": n - 1 - p2,
        "price_drop": round((cv[p2] / cv[p1] - 1) * 100, 2),   # 负 = 新低幅度
        "diff_lift": round(float(dv[p2] - dv[p1]), 3),
        "rebound": bool(cv[-1] > cv[p2] and dv[-1] > dv[p2]),  # 已反弹确认
    }


def detect_5m_gc(df5, fresh=24):
    """5m MACD 零轴上金叉: 最近一次 DIFF 上穿 DEA, 交叉处 DEA>0 (零轴上方), 且在最近 fresh 根内。

    返回 {gc_ts, gc_age, gc_diff, gc_dea, gc_rising} 或 None。fresh 单位 5m 根 (默认 24=2 小时)。
    """
    if df5 is None or len(df5) < 35:
        return None
    c = df5["C"].astype(float)
    diff = c.ewm(span=12, adjust=False).mean() - c.ewm(span=26, adjust=False).mean()
    dea = diff.ewm(span=9, adjust=False).mean()
    dv, ev, n = diff.values, dea.values, len(diff)
    gc = None
    for i in range(1, n):
        if dv[i - 1] <= ev[i - 1] and dv[i] > ev[i]:
            gc = i                                 # 最近一次金叉
    if gc is None or gc < n - fresh:
        return None
    if not (ev[gc] > 0):                           # 必须零轴上方
        return None
    return {
        "gc_ts": str(df5["ts"].iloc[gc])[:16],
        "gc_age": n - 1 - gc,
        "gc_diff": round(float(dv[-1]), 3),
        "gc_dea": round(float(ev[gc]), 3),
        "gc_rising": bool(n >= 4 and dv[-1] > dv[-3]),   # DIFF 当下上扬
    }


def detect_div(df5, pivot_k=3, fresh_days=3, gc_fresh=24):
    """div 法 = 30m MACD 底背离 + 5m MACD 零轴上金叉 (任一命中即触发)。

    30m 背离: 回测验证过的次日反弹信号 (价 LL + DIFF HL)。
    5m 零轴金叉: 短线动能恢复 (DIFF 上穿 DEA 且交叉在零轴上方)。
    两信号共振 +15。返回 {div_score, div_tf, ...} 或 None。
    """
    bars = resample_intraday(df5, 30)
    div = detect_bottom_div(bars, k=pivot_k, sep=16, fresh=8 * fresh_days) if bars is not None else None
    gc = detect_5m_gc(df5, gc_fresh)
    if not (div or gc):
        return None
    tags, score = [], 0.0
    if div:
        tags.append("30m背离")
        score += 45
        if div["rebound"]:
            score += 10
    if gc:
        tags.append("5m金叉")
        score += 30
        if gc["gc_rising"]:
            score += 10
    if div and gc:
        score += 15                                 # 共振
    out = {"div_score": min(round(score, 1), 100.0), "div_tf": "+".join(tags)}
    if div:
        out.update(div_p2=div["p2_ts"], div_drop=div["price_drop"],
                   div_lift=div["diff_lift"], div_rebound=div["rebound"])
    if gc:
        out.update(gc_diff=gc["gc_diff"], gc_dea=gc["gc_dea"], gc_age=gc["gc_age"],
                   gc_ts=gc["gc_ts"], gc_rising=gc["gc_rising"])
    return out


_tls = threading.local()


def thread_conn():
    """线程局部 TDengine 连接 (div 逐股查 5m, 每工作线程复用一个 conn)。"""
    c = getattr(_tls, "conn", None)
    if c is None:
        c = connect()
        _tls.conn = c
    return c


# ---------------------------------------------------------------------------
# 底背离回测
# ---------------------------------------------------------------------------
_BACKTEST_HOLD = [1, 5, 10, 20]   # 持有期 (交易日)


def detect_all_div(bars, k=3, sep=4):
    """扫描全部历史底背离点: 所有相邻波谷对 (p1<p2) 满足价 LL + DIFF HL → 返回 p2 索引列表。

    回测用 (detect_bottom_div 只取最近一对)。p2 需 k 根右确认, 回测入场设在 p2+k (无未来函数)。
    """
    if bars is None or len(bars) < 35:
        return []
    c = bars["C"].astype(float)
    diff = c.ewm(span=12, adjust=False).mean() - c.ewm(span=26, adjust=False).mean()
    cv, dv = c.values, diff.values
    piv = _pivot_lows(cv, k, sep)
    return [piv[i] for i in range(1, len(piv))
            if cv[piv[i]] < cv[piv[i - 1]] and dv[piv[i]] > dv[piv[i - 1]]]


def _backtest_one(df5, k=3):
    """单股回测: 30m 底背离 + 5m 零轴金叉 → 各持有期前向收益 + 全 bar 基线。

    返回 {'30m背离': {...}, '5m金叉': {...}} (各含 sig/base/n_sig) 或 None。
    入场均无未来函数: 30m 背离在 p2+k 根后; 5m 金叉在交叉后下一根。
    """
    out = {}
    # --- 30m 底背离 (bpd=8) ---
    bars = resample_intraday(df5, 30)
    if bars is not None and len(bars) >= 35:
        cv = bars["C"].astype(float).values
        n = len(cv)
        p2s = detect_all_div(bars, k, 16)
        sig = {h: [] for h in _BACKTEST_HOLD}
        for p2 in p2s:
            entry = p2 + k
            if entry >= n:
                continue
            pe = cv[entry]
            for h in _BACKTEST_HOLD:
                jb = entry + h * 8
                if jb < n and pe > 0:
                    sig[h].append(cv[jb] / pe - 1)
        base = {h: [] for h in _BACKTEST_HOLD}
        for entry in range(k, n):
            pe = cv[entry]
            if pe <= 0:
                continue
            for h in _BACKTEST_HOLD:
                jb = entry + h * 8
                if jb < n:
                    base[h].append(cv[jb] / pe - 1)
        out["30m背离"] = {"sig": sig, "base": base, "n_sig": len(p2s)}
    # --- 5m 零轴金叉 (1 交易日 = 48 根) ---
    if df5 is not None and len(df5) >= 35:
        c = df5["C"].astype(float)
        diff = c.ewm(span=12, adjust=False).mean() - c.ewm(span=26, adjust=False).mean()
        dea = diff.ewm(span=9, adjust=False).mean()
        cv5, dv, ev, n5 = c.values, diff.values, dea.values, len(c)
        gcs = [i for i in range(1, n5) if dv[i - 1] <= ev[i - 1] and dv[i] > ev[i] and ev[i] > 0]
        sig = {h: [] for h in _BACKTEST_HOLD}
        for gc in gcs:
            entry = gc + 1                       # 交叉确认后下一根入场
            if entry >= n5:
                continue
            pe = cv5[entry]
            for h in _BACKTEST_HOLD:
                jb = entry + h * 48
                if jb < n5 and pe > 0:
                    sig[h].append(cv5[jb] / pe - 1)
        base = {h: [] for h in _BACKTEST_HOLD}
        for entry in range(n5):
            pe = cv5[entry]
            if pe <= 0:
                continue
            for h in _BACKTEST_HOLD:
                jb = entry + h * 48
                if jb < n5:
                    base[h].append(cv5[jb] / pe - 1)
        out["5m金叉"] = {"sig": sig, "base": base, "n_sig": len(gcs)}
    return out or None


def backtest_div_main(args):
    """对标的池做 30m 底背离 + 5m 零轴金叉回测, 估算各持有期收益率 (vs 全 bar 基线)。"""
    conn = connect()
    if args.codes:
        pool = [parse_code(c) for c in args.codes]
    elif args.all:
        pool = all_mainboard_codes(conn)
    else:                                  # 默认 zxg (含 --zxg 或无参)
        pool = [parse_code(c) for c in zxg_codes()]
    if args.limit:
        pool = pool[:args.limit]
    print(f"[backtest-div] {len(pool)} 只, 窗口 {args.bt_days} 交易日, 持有期 {_BACKTEST_HOLD} 天")

    adj_by_mc = batch_fetch_adjust(conn, pool)

    def _task(m, c):
        df5 = fetch_kline(thread_conn(), m, c, cycle="5m", days=args.bt_days, min_rows=200)
        if df5 is None:
            return None
        return _backtest_one(apply_qfq(df5, adj_by_mc.get((m, c))), args.div_pivot)

    keys = ("30m背离", "5m金叉")
    agg = {k: {"sig": {h: [] for h in _BACKTEST_HOLD},
               "base": {h: [] for h in _BACKTEST_HOLD}, "n_sig": 0} for k in keys}
    done = 0
    with ThreadPoolExecutor(max_workers=args.jobs) as ex:
        futs = {ex.submit(_task, m, c): (m, c) for m, c in pool}
        for f in as_completed(futs):
            try:
                res = f.result()
            except Exception as e:
                sys.stderr.write(f"[warn] bt 异常 {futs[f]}: {e}\n")
                continue
            done += 1
            if not res:
                continue
            for k, d in res.items():
                for h in _BACKTEST_HOLD:
                    agg[k]["sig"][h].extend(d["sig"][h])
                    agg[k]["base"][h].extend(d["base"][h])
                agg[k]["n_sig"] += d["n_sig"]
    print(f"[backtest-div] {done}/{len(pool)} 只有足够 5m 数据")

    def stats(arr):
        if not arr:
            return None
        a = np.array(arr)
        return {"n": len(a), "win": float((a > 0).mean() * 100),
                "mean": float(a.mean() * 100), "med": float(np.median(a) * 100)}

    print(f"\n=== 分钟MACD信号回测 (30m底背离 / 5m零轴金叉; 持有期单位: 交易日) ===")
    print(f"{'信号':<8}{'信号数':>6} | {'+1d 胜/均/中位':>22} | {'+5d':>22} | {'+10d':>22} | {'+20d':>22}")
    for k in keys:
        d = agg[k]
        cells = []
        for h in _BACKTEST_HOLD:
            s = stats(d["sig"][h]); b = stats(d["base"][h])
            cells.append((f"{s['win']:.0f}%/{s['mean']:+.1f}/{s['med']:+.1f}(基{b['mean']:+.1f})")
                         if s and b else "-")
        print(f"{k:<8}{d['n_sig']:>6} | {cells[0]:>22} | {cells[1]:>22} | {cells[2]:>22} | {cells[3]:>22}")
    print("\n说明: 胜/均/中位 = 胜率%/平均收益%/收益中位数% (基 = 全 bar 随机入场同持有期均收益%)")
    print("      30m背离入场=p2右确认k根后; 5m金叉入场=交叉后下一根; 均无未来函数")


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
def _selected(r, args):
    """是否入选 (OR): 任一命中方法达该法阈值即入选。strategy 为多法标签 (如 '漏斗+30m背离')。"""
    s = r["strategy"]
    if "漏斗" in s and r.get("leader_score", 0) >= args.min_score:
        return True
    if "二次穿越" in s and r.get("dc_score", 0) >= args.dc_min_score:
        return True
    if ("背离" in s or "金叉" in s) and r.get("div_score", 0) >= args.div_min_score:
        return True
    return False


def _base_row(last, rets=None):
    """从日线末行抽取共性字段 (漏斗未命中的 dc/div-only 票, leader 模式下补齐指标列)。"""
    lr = last.get("ret_250")
    rps = (rets < lr).mean() * 100 if (rets is not None and lr is not None
                                       and not pd.isna(lr)) else 0.0
    return {
        "RPS": round(rps, 1),
        "price": round(float(last["C"]), 2),
        "ret_250": round(float(lr), 1) if lr is not None and not pd.isna(lr) else 0,
        "near_high": round(float(last.get("near_high", 0) or 0), 3),
        "RSI14": round(float(last["RSI14"]), 1) if "RSI14" in last.index and not pd.isna(last["RSI14"]) else 0,
        "ADX": round(float(last["ADX"]), 1) if "ADX" in last.index and not pd.isna(last["ADX"]) else 0,
        "CCI": round(float(last["CCI"]), 1) if "CCI" in last.index and not pd.isna(last["CCI"]) else 0,
        "ATR_pct": round(float(last["ATR_pct"]), 2) if "ATR_pct" in last.index
        and not pd.isna(last["ATR_pct"]) else None,
        "MA5": round(float(last["MA5"]), 2) if "MA5" in last.index and not pd.isna(last["MA5"]) else 0,
        "MA20": round(float(last["MA20"]), 2) if "MA20" in last.index and not pd.isna(last["MA20"]) else 0,
    }


def main():
    ap = argparse.ArgumentParser(description="seiver 选股筛子 (龙头漏斗 + 周线二次穿越 + 30m底背离/5m零轴金叉)")
    ap.add_argument("--codes", nargs="*", help="指定代码")
    ap.add_argument("--zxg", action="store_true", help="自选股")
    ap.add_argument("--all", action="store_true", help="全主板")
    ap.add_argument("--top", type=int, default=20, help="输出 top N")
    ap.add_argument("--min-score", type=float, default=40, help="漏斗最低评分")
    ap.add_argument("--rps", type=float, default=80, help="RPS 最低分位 (默认 80)")
    ap.add_argument("--diagnostic", action="store_true",
                    help="显示 RPS 通过但其他过滤未通过的股票 (诊断模式)")
    ap.add_argument("--limit", type=int, help="最多分析多少只 (调试)")
    ap.add_argument("--output-dir", default="output/seiver", help="输出目录")
    ap.add_argument("--jobs", type=int, default=8, help="并发线程数 (默认 8)")
    ap.add_argument("--methods", nargs="+", default=["leader", "dc", "div"],
                    choices=["leader", "dc", "div"],
                    help="选股方法 (默认 leader dc div 三法 OR 合并; "
                         "leader=龙头漏斗, dc=周线二次穿越, div=30m底背离+5m零轴金叉)")
    # 周线 MACD 二次穿越参数
    ap.add_argument("--dc-fresh", type=int, default=4, help="二次穿越新鲜度: g2 在最近 N 周内 (默认 4)")
    ap.add_argument("--dc-lookback", type=int, default=30, help="二次穿越回溯窗口周数 (默认 30)")
    ap.add_argument("--dc-zero-band", type=float, default=0.02,
                    help="零轴带: g2 处 DEA >= -band×价 (默认 0.02=2%%, 近/上零轴)")
    ap.add_argument("--dc-vol-ratio", type=float, default=1.3,
                    help="放量比: 当周量 > ratio×洗盘期均量 (默认 1.3)")
    ap.add_argument("--dc-min-score", type=float, default=58,
                    help="二次穿越最低分 (默认 58 = 结构50 + 至少一项确认)")
    ap.add_argument("--dc-selftest", action="store_true", help="合成数据自检二次穿越检测后退出")
    # div: 30m 底背离 + 5m 零轴金叉
    ap.add_argument("--div-days", type=int, default=30, help="div 取近 N 个交易日 5m 线 (默认 30)")
    ap.add_argument("--div-pivot", type=int, default=3, help="30m底背离波谷确认窗口 ±k 根 (默认 3)")
    ap.add_argument("--div-fresh", type=int, default=3, help="30m背离新鲜度: p2 在最近 N 个交易日内 (默认 3)")
    ap.add_argument("--gc-fresh", type=int, default=24, help="5m金叉新鲜度: 交叉在最近 N 根 5m 内 (默认 24=2小时)")
    ap.add_argument("--div-min-score", type=float, default=40,
                    help="div 最低分 (默认 40: 30m背离=45直接达标, 5m金叉=30需DIFF上扬+10)")
    ap.add_argument("--div-selftest", action="store_true", help="合成数据自检底背离检测后退出")
    ap.add_argument("--backtest-div", action="store_true", help="回测 30m底背离+5m零轴金叉 收益率后退出")
    ap.add_argument("--bt-days", type=int, default=120, help="回测窗口 (交易日 5m 线, 默认 120)")
    args = ap.parse_args()

    if args.dc_selftest:
        _dc_selftest()
        return
    if args.div_selftest:
        _div_selftest()
        return
    if args.backtest_div:
        backtest_div_main(args)
        return

    run_leader = "leader" in args.methods
    run_dc = "dc" in args.methods
    run_div = "div" in args.methods

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

    # 板块动量 (仅 leader 用: score_stock 取板块涨幅)
    sec_mom = {}
    if run_leader:
        print("[sector] computing momentum...")
        sec_mom = sector_momentum(conn)
        for code, ret in sorted(sec_mom.items(), key=lambda x: -x[1]):
            print(f"  {pad(SECTOR_INDICES.get(code, code), 8)} {code}: {ret:+.2f}% (20d)")

    # 批量拉日线 (一次查询替代 N 次) + 批量复权事件
    min_rows = 252 if run_leader else 180   # leader 需 ret_250; dc 需 >=35 周
    print(f"[fetch] 批量拉取 {len(pool)} 只日线 (min_rows={min_rows}, methods={'+'.join(args.methods)})...")
    klines = batch_fetch_klines(conn, pool, days=400, min_rows=min_rows)
    print(f"[fetch] {len(klines)} 只有足够数据 (>={min_rows} 行)")
    adj_by_mc = batch_fetch_adjust(conn, pool)
    print(f"[fetch] {len(adj_by_mc)} 只有除权事件 (应用前复权)")

    all_features = []
    with ThreadPoolExecutor(max_workers=args.jobs) as ex:
        futs = [ex.submit(_compute, m, c, klines[(m, c)], adj_by_mc.get((m, c)), run_leader)
                for m, c in pool if (m, c) in klines]
        for f in as_completed(futs):
            try:
                all_features.append(f.result())
            except Exception as e:
                sys.stderr.write(f"[warn] 指标计算异常: {e}\n")
    print(f"[fetch] {len(all_features)} 只完成预处理"
          + (" (含日线指标)" if run_leader else " (仅复权, dc-only)"))
    if not all_features:
        print("[error] no data", file=sys.stderr)
        sys.exit(1)

    # 全市场 RPS 排名 (ret_250 的 percentile) — 仅 leader
    results = []
    diagnostic = []
    rets = None
    if run_leader:
        all_ret = []
        for m, c, df in all_features:
            if len(df) > 1 and not pd.isna(df.iloc[-1].get("ret_250")):
                all_ret.append((m, c, df.iloc[-1]["ret_250"]))
        if not all_ret:
            print("[error] no valid ret_250", file=sys.stderr)
            sys.exit(1)
        rets = np.array([x[2] for x in all_ret])

        # 评分
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

    # ---- 周线 MACD 二次穿越 ----
    dc_map = {}
    if run_dc:
        print("[dc] 检测周线 MACD 二次穿越...")

        def _dc_task(df):
            return detect_double_cross(to_weekly(df), args.dc_fresh, args.dc_lookback,
                                       args.dc_zero_band, args.dc_vol_ratio)

        with ThreadPoolExecutor(max_workers=args.jobs) as ex:
            futs = {ex.submit(_dc_task, df): (m, c) for m, c, df in all_features}
            for f in as_completed(futs):
                sig = f.result()
                if sig:
                    dc_map[futs[f]] = sig
        print(f"[dc] {len(dc_map)} 只触发二次穿越")

    # ---- div: 30m 底背离 + 5m 零轴金叉 (逐股查 5m, 线程局部 conn) ----
    div_map = {}
    if run_div:
        print("[div] 检测 30m MACD 底背离 + 5m 零轴金叉...")
        feat_set = {(m, c) for m, c, _ in all_features}

        def _div_task(m, c):
            df5 = fetch_kline(thread_conn(), m, c, cycle="5m", days=args.div_days, min_rows=200)
            if df5 is None:
                return None
            return detect_div(apply_qfq(df5, adj_by_mc.get((m, c))),
                              args.div_pivot, args.div_fresh, args.gc_fresh)

        with ThreadPoolExecutor(max_workers=args.jobs) as ex:
            futs = {ex.submit(_div_task, m, c): (m, c) for m, c in pool if (m, c) in feat_set}
            for f in as_completed(futs):
                mc = futs[f]
                try:
                    sig = f.result()
                except Exception as e:
                    sys.stderr.write(f"[warn] div 异常 {mc}: {e}\n")
                    continue
                if sig:
                    div_map[mc] = sig
        print(f"[div] {len(div_map)} 只触发 (30m背离/5m金叉)")

    # ---- 统一合并 (leader/dc/div 任一命中即入选; strategy 为多法标签) ----
    leader_by = {(r["market"], r["code"]): r for r in results}
    feat_by_mc = {(m, c): df for m, c, df in all_features}
    results = []
    for key in set(leader_by) | set(dc_map) | set(div_map):
        m, c = key
        lr = leader_by.get(key)
        dcs = dc_map.get(key)
        dvs = div_map.get(key)
        tags = []
        row = {"market": m, "code": c, "name": names.get(key, ""),
               "sector": SECTOR_INDICES.get(get_sector_for_code(c), get_sector_for_code(c))}
        if lr:
            tags.append("漏斗")
            row.update(lr)
            row.pop("strategy", None)
            row["leader_score"] = lr["score"]
        if dcs:
            tags.append("二次穿越")
            row.update(dcs)
        if dvs:                          # div 子信号标签 (30m背离 / 5m金叉 / 共振)
            tags.append(dvs["div_tf"])
            row.update(dvs)
        if not lr and run_leader:    # 非漏斗命中: leader 模式下从日线补齐指标列
            df = feat_by_mc.get(key)
            if df is not None:
                row.update(_base_row(df.iloc[-1], rets))
        cand = []
        if lr:
            cand.append(lr["score"])
        if dcs:
            cand.append(dcs["dc_score"])
        if dvs:
            cand.append(dvs["div_score"])
        row["score"] = round(max(cand), 1) if cand else 0.0
        row["strategy"] = "+".join(tags)
        results.append(row)

    # 排序
    results.sort(key=lambda x: -x["score"])

    # 输出
    os.makedirs(args.output_dir, exist_ok=True)
    df_out = pd.DataFrame(results)
    if not df_out.empty:
        if "ATR_pct" in df_out.columns:   # dc-only 未算日线指标, 无 ATR
            df_out["stop_loss"] = df_out["price"] * (1 - df_out["ATR_pct"].fillna(0) / 100 * 2)
        cols = ["market", "code", "name", "strategy", "score", "RPS", "price", "ret_250", "near_high",
                "RSI14", "ADX", "CCI", "ATR_pct", "stop_loss", "sector",
                "momentum", "high_score", "ma_align", "rsi", "cci", "pocket_pivot",
                "dc_score", "dc_diff", "dc_dea", "dc_g2", "dc_age", "dc_zero", "dc_vol", "dc_ma",
                "div_score", "div_tf", "div_p2", "div_drop", "div_lift", "div_rebound",
                "gc_diff", "gc_dea", "gc_age", "gc_ts", "gc_rising"]
        df_out = df_out[[c for c in cols if c in df_out.columns]]
        # 日期戳 CSV (浮点保留3位)
        csv_file = os.path.join(args.output_dir, f"seiver-{time.strftime('%Y%m%d')}.csv")
        df_out.round(3).to_csv(csv_file, index=False)
        # 兼容旧名
        df_out.to_csv(os.path.join(args.output_dir, "seiver.csv"), index=False)

    # 主表 (龙头漏斗视角): 仅 leader 模式有日线指标列, dc-only 跳过直接看二次穿越明细
    filtered = [r for r in results if _selected(r, args)]
    if run_leader:
        parts = []
        if run_leader:
            parts.append(f"漏斗score>={args.min_score}")
        if run_dc:
            parts.append(f"二次穿越score>={args.dc_min_score}")
        if run_div:
            parts.append(f"div(30m背离/5m金叉)score>={args.div_min_score}")
        print(f"\n=== 板块龙头候选 (top {args.top}: {' 或 '.join(parts)}) ===")
        if not filtered:
            print("  (无通过过滤的股票)")
        else:
            print(pad("排名", 4, ">") + " " + pad("策略", 18) + " " + pad("代码", 10) + " " + pad("名称", 15)
                  + " " + pad("评分", 6, ">") + " " + pad("RPS", 6, ">") + " " + pad("价格", 8, ">")
                  + " " + pad("250日%", 8, ">") + " " + pad("近新高", 6, ">") + " " + pad("RSI", 5, ">")
                  + " " + pad("ADX", 5, ">") + " " + pad("CCI", 6, ">") + " " + pad("ATR%", 5, ">")
                  + " " + pad("止损位", 8, ">") + " " + pad("板块", 4))
            for i, r in enumerate(filtered[:args.top], 1):
                nm = r.get('name', '') or ''
                atr = r.get("ATR_pct", 0) or 0
                stop = r["price"] * (1 - atr / 100 * 2)
                print(f"{i:4d} {pad(r.get('strategy', ''), 18)} {r['market']}{r['code']:<8} {pad(nm, 15)} "
                      f"{r['score']:6.1f} {r['RPS']:6.1f} "
                      f"{r['price']:8.2f} {r['ret_250']:8.1f} {r['near_high']:6.3f} "
                      f"{r['RSI14']:5.1f} {r['ADX']:5.1f} {r['CCI']:6.1f} "
                      f"{atr:5.2f} {stop:8.2f} {r.get('sector', '')}")

    # 二次穿越明细 (含与它法共振)
    dc_rows = [r for r in results if "二次穿越" in r["strategy"]]
    if dc_rows and run_dc:
        dc_rows.sort(key=lambda x: -x.get("dc_score", 0))
        print(f"\n=== 周线MACD二次穿越 ({len(dc_rows)} 只) ===")
        print(f"{'策略':<14}{'代码':<11}{'名称':<16}{'DC分':>5} {'DIFF':>7} {'DEA':>7} "
              f"{'二次金叉':>10} {'龄':>3} {'零轴':<7}{'量':<8}{'周均线':<5}")
        for r in dc_rows:
            print(f"{r['strategy']:<14}{r['market']}{r['code']:<8}{pad(r.get('name', ''), 16)}"
                  f"{r['dc_score']:5.1f} {r['dc_diff']:7.3f} {r['dc_dea']:7.3f} "
                  f"{r['dc_g2']:>10} {r['dc_age']:3d} {pad(r['dc_zero'], 7)}{pad(r['dc_vol'], 8)}"
                  f"{pad(r['dc_ma'], 5)}")

    # 30m 底背离明细
    div_rows = [r for r in results if "背离" in r["strategy"]]
    if div_rows and run_div:
        div_rows.sort(key=lambda x: -x.get("div_score", 0))
        print(f"\n=== 30m MACD底背离 ({len(div_rows)} 只) ===")
        print(f"{'策略':<20}{'代码':<11}{'名称':<16}{'DIV分':>5} {'背离低':>16} {'价跌%':>6} {'DIFF抬':>7} {'反弹':<4}")
        for r in div_rows:
            print(f"{r['strategy']:<20}{r['market']}{r['code']:<8}{pad(r.get('name', ''), 16)}"
                  f"{r['div_score']:5.1f} {r['div_p2']:>16} {r['div_drop']:6.2f} {r['div_lift']:7.3f} "
                  f"{'是' if r['div_rebound'] else '否'}")

    # 5m 零轴金叉明细
    gc_rows = [r for r in results if "金叉" in r["strategy"]]
    if gc_rows and run_div:
        gc_rows.sort(key=lambda x: -x.get("div_score", 0))
        print(f"\n=== 5m MACD零轴金叉 ({len(gc_rows)} 只) ===")
        print(f"{'策略':<20}{'代码':<11}{'名称':<16}{'DIV分':>5} {'金叉时':>16} {'DIFF':>7} {'DEA':>7} {'龄':>3} {'上扬':<4}")
        for r in gc_rows:
            print(f"{r['strategy']:<20}{r['market']}{r['code']:<8}{pad(r.get('name', ''), 16)}"
                  f"{r['div_score']:5.1f} {r['gc_ts']:>16} {r['gc_diff']:7.3f} {r['gc_dea']:7.3f} "
                  f"{r['gc_age']:3d} {'是' if r.get('gc_rising') else '否'}")

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

    print(f"\n[output] → {args.output_dir}/seiver.csv")
    n_dc = sum(1 for r in results if "二次穿越" in r["strategy"])
    n_div = sum(1 for r in results if "背离" in r["strategy"])
    n_gc = sum(1 for r in results if "金叉" in r["strategy"])
    print(f"[summary] {len(results)} passed ({len(filtered)} 入选: methods={'+'.join(args.methods)}), "
          f"二次穿越 {n_dc} / 30m背离 {n_div} / 5m金叉 {n_gc} 只")


def _dc_selftest():
    """合成周线自检: 二次穿越结构应检出, 单调下跌不应检出。

    用宽松阈值 (fresh/zero_band) 验证「两次金叉夹死叉 + 最近金叉」的结构识别,
    不验证默认阈值的严格调参。
    """
    def mk(closes, vols):
        idx = pd.date_range(end=pd.Timestamp.today().normalize(),
                            periods=len(closes), freq="W-FRI")
        return pd.DataFrame({"O": closes, "H": closes, "L": closes,
                             "C": closes, "V": vols, "amount": closes}, index=idx)
    # 跌 → 反弹(g1) → 深跌并横盘(死叉/洗盘) → 再反弹(g2) → 收尾
    p = ([14 - i * 0.45 for i in range(15)]          # 下跌
         + [7.85 + i * 0.40 for i in range(1, 9)]     # 反弹 g1
         + [11 - i * 0.30 for i in range(1, 9)]       # 回落
         + [8.6] * 4                                  # 横盘: 逼 DIFF 下穿 DEA (死叉)
         + [8.6 + i * 0.45 for i in range(1, 9)]      # 再反弹 g2
         + [12.2] * 2)                                # 收尾
    v = [1e7] * len(p)
    for k in range(3):                                # 末 3 周放量
        v[-1 - k] = 1.6e7 * (k + 1)
    pos = detect_double_cross(mk(p, v), fresh=15, lookback=40, zero_band=0.5)
    neg = detect_double_cross(mk([12 - i * 0.1 for i in range(60)], [1e7] * 60),
                              fresh=15, lookback=40, zero_band=0.5)
    assert pos, "二次穿越结构应被检出"
    assert neg is None, "单调下跌不应被检出"
    print(f"[dc-selftest] OK  检出: dc_score={pos['dc_score']} {pos['dc_zero']}/{pos['dc_vol']}/MA{pos['dc_ma']} "
          f"g2={pos['dc_g2']} 龄={pos['dc_age']}  | 单调下跌: 未检出")


def _div_selftest():
    """合成底背离自检: 价创新低但 DIFF 抬高应检出; 单调下跌不应检出。

    直接造 120m 序列喂 detect_bottom_div (验证 pivot LL + DIFF HL), 不经 5m 重采样。
    """
    def bars(closes):
        idx = pd.date_range(end=pd.Timestamp.today().normalize(),
                            periods=len(closes), freq="2h")
        return pd.DataFrame({"O": closes, "H": closes, "L": closes, "C": closes,
                             "V": [1e6] * len(closes), "ts": idx})
    # 急跌(深 DIFF 负) → 反弹 → 缓跌出新低(DIFF 没上一轮那么负) → 收尾贴近低点
    p = ([20 - i * 0.55 for i in range(18)]      # 急跌 20→10.6 (low1, DIFF 深负)
         + [10.6 + i * 0.30 for i in range(1, 11)]   # 反弹 10.6→13.5
         + [13.5 - i * 0.32 for i in range(1, 11)]   # 缓跌 13.5→10.4 (<10.6 新低, DIFF 抬高)
         + [10.5, 10.6, 10.8])                       # 收尾: 贴近 low2, age 小
    pos = detect_bottom_div(bars(p), k=3, sep=6, fresh=30)
    neg = detect_bottom_div(bars([20 - i * 0.25 for i in range(42)]), k=3, sep=6, fresh=30)
    assert pos, "底背离结构 (价LL+DIFF HL) 应被检出"
    assert neg is None, "单调下跌不应被检出"
    print(f"[div-selftest] OK  检出: 价跌={pos['price_drop']}% DIFF抬={pos['diff_lift']} "
          f"低点={pos['p2_ts']} 龄={pos['age']} 反弹={pos['rebound']}  | 单调下跌: 未检出")


if __name__ == "__main__":
    main()
