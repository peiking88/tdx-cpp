#!/usr/bin/env python3
"""
================================================================================
 A股后市推演 market-struct.py — 上证综指 / 创业板指 / 科创50
================================================================================

前提论断（《浅析A股大结构》pdf，趋势之美07 2026-08-20，本脚本不做历史验证，
直接作为推演公理使用）:
  ① A股自 2015 大顶起处于「熊市反弹大结构」（大B浪），日本同构 B 浪全长
     90 个月（1992-09 → 2000-03）;
  ② 2019-2021 与 2024-2026 为大B浪中两个同构的反弹子浪（浪1 / 浪2）,
     中间的调整浪为反弹结构内部调整（深综指口径 26 个月）;
  ③ 10Y 国债利率长期下降楔形是经济周期真相之锚; 日本 1998 楔形破位后
     B 浪仍延续约 20 个月才见 2000-03 大顶。

推演规则（全部可由数据复算）:
  时间目标  T_struct = 浪2底 + 浪1全长（同构时长）
  时钟目标  T_clock  = 浪1底 + 90 个月（日本B浪全长）
  空间目标  P_struct = 浪2底 × (1 + 浪1涨幅)（同构涨幅）
  终结判定  时间窗关闭（now ≥ min/max(T_struct, T_clock)）× 回撤破位
            （距浪2顶回撤 ≤ -max(15%, 浪1内最大回撤)）
  终结之后  斐波那契支撑（浪2幅度 38.2/50/61.8%）; 日本C浪参照
            （2000-03→2003-04 -63.6%/36个月, 按「各指数A浪深度/日本A浪深度」
            折算, 日本史实、文章未涉及, 仅作极端边界）

数据来源:
  指数日线 ← 本仓 TDengine（REST 网关, 默认 localhost:6041）
  10Y 利率 ← 东方财富数据中心;  日本里程碑 ← 公开史实常量

输出:
  output/market-struct/market-struct-YYYYMMDD.md  推演报告
  output/market-struct/market-struct-YYYYMMDD.png 四联图

用法:
  python3 scripts/market-struct.py               # 全量推演
  python3 scripts/market-struct.py --no-chart    # 不出图

依赖: pandas / numpy / requests（图另需 matplotlib），无需 taosws。
================================================================================
"""

import argparse
import os
import sys
from datetime import date

import numpy as np
import pandas as pd
import requests

# ── 推演参数 ──
# 日本: B浪90个月（文章给定）; C浪与利率破位后延（日本史实, 文章未涉及, 仅作参照）
JAPAN = {
    "b_months": 90,            # 1992-09 → 2000-03 大B浪全长
    "a_drop": -0.6357,         # 日经 A浪 38957 → 14194
    "c_drop": -0.6357,         # 日经 C浪 2000-03 20833 → 2003-04 7607
    "c_months": 36,            # C浪时长
    "post_break_b_months": 20, # 1998 利率楔形破位 → 2000-03 B浪顶
}

# ── 指数档案: 浪形枢轴搜索窗口（月线窗口极值 → 回日线取精确日期） ──
# w1_low 浪1底 / w1_top 浪1顶 / w2_low 浪2底（调整浪底）
INDEX_PROFILES = [
    {
        "code": "sh000001", "name": "上证综指",
        "windows": [("top2015", "2015-01-01", "2015-12-31", "H"),
                    ("w1_low", "2018-06-01", "2019-02-28", "L"),
                    ("w1_top", "2021-01-01", "2021-12-31", "H"),
                    ("w2_low", "2024-01-01", "2024-09-30", "L")],
        "chart_from": "2016-01-01",
    },
    {
        "code": "sz399006", "name": "创业板指",
        "windows": [("top2015", "2015-01-01", "2015-12-31", "H"),
                    ("w1_low", "2018-06-01", "2019-02-28", "L"),
                    ("w1_top", "2021-01-01", "2021-12-31", "H"),
                    ("w2_low", "2024-01-01", "2024-09-30", "L")],
        "chart_from": "2016-01-01",
    },
    {
        "code": "sh000688", "name": "科创50",
        # 2019-12-31 基日发布: 无 2015 顶/A浪; 浪1底取上市初低点区（含 2020-03 疫情低）
        "windows": [("w1_low", "2019-12-31", "2020-06-30", "L"),
                    ("w1_top", "2021-06-01", "2021-12-31", "H"),
                    ("w2_low", "2024-01-01", "2024-09-30", "L")],
        "chart_from": "2019-12-31",
    },
]

TDENGINE_REST = os.environ.get(
    "TDENGINE_REST", "http://localhost:6041/rest/sql/tdx")


# ============================================================================
# 数据获取
# ============================================================================
def rest_query(sql):
    """TDengine REST 查询 → DataFrame（列名取自 column_meta）。"""
    r = requests.post(TDENGINE_REST, data=sql.encode("utf-8"),
                      auth=("root", "taosdata"), timeout=60)
    j = r.json()
    if j.get("code"):
        raise RuntimeError(f"TDengine: {j.get('desc')}")
    cols = [c[0] for c in j["column_meta"]]
    return pd.DataFrame(j["data"], columns=cols)


def fetch_index_daily(code):
    """指数全史日线 → DataFrame[d, O, H, L, C]（d=北京日期）。"""
    df = rest_query(
        f"SELECT ts, open, high, low, close FROM k_{code}_1d ORDER BY ts")
    df["d"] = pd.to_datetime(df["ts"].str[:10])
    return df[["d", "open", "high", "low", "close"]].rename(
        columns={"open": "O", "high": "H", "low": "L", "close": "C"})


def fetch_yield10y():
    """中国 10Y 国债收益率日序列（东财 RPTA_WEB_TREASURYYIELD, 翻页到 2010）。

    返回 (Series[date→%], None) 或 (None, 错误信息)。
    """
    url = "https://datacenter.eastmoney.com/api/data/get"
    token = "894050c76af8597a853f5b408b759f5d"
    rows = []
    for page in range(1, 25):  # 上限防御: 全史约 19 页
        params = {"type": "RPTA_WEB_TREASURYYIELD", "sty": "ALL",
                  "st": "SOLAR_DATE", "sr": "-1", "token": token,
                  "p": page, "ps": 500, "pageNo": page, "pageNum": page}
        j = requests.get(url, params=params, timeout=20).json()
        data = (j.get("result") or {}).get("data") or []
        if not data:
            break
        rows += data
        if str(data[-1]["SOLAR_DATE"]) < "2010-01-01":
            break
    if not rows:
        return None, "东财接口无返回"
    df = pd.DataFrame(rows)
    s = (df.assign(date=pd.to_datetime(df["SOLAR_DATE"]))
           .set_index("date")["EMM00166466"]   # 中国国债收益率10年
           .astype(float).sort_index().dropna())
    return s, None


# ============================================================================
# 月线 + 浪形枢轴
# ============================================================================
def to_monthly(df):
    """日线 → 月线。返回 (m, hd, ld):
    m  — 逐月 DataFrame[H, L, C]（index=月首）, hd/ld — 各月最高/最低价精确日。
    """
    g = df.copy()
    g["m"] = g["d"].values.astype("datetime64[M]")
    m = g.groupby("m").agg(H=("H", "max"), L=("L", "min"), C=("C", "last"))
    hd = g.loc[g.groupby("m")["H"].idxmax()].set_index("m")["d"]
    ld = g.loc[g.groupby("m")["L"].idxmin()].set_index("m")["d"]
    return m, hd, ld


def months_between(a, b):
    """两个日期间的自然月数（b - a）。"""
    return (b.year - a.year) * 12 + (b.month - a.month)


def add_months(ts, n):
    """Timestamp 加 n 个月（保留日, 溢出取月末）。"""
    return ts + pd.DateOffset(months=n)


def find_pivots(m, hd, ld, windows):
    """按档案窗口极值定位浪形枢轴 → dict[键 → {date, value}]。"""
    out = {}
    for key, lo, hi, mode in windows:
        w = m.loc[lo:hi]
        if w.empty:
            continue
        if mode == "H":
            mi = w["H"].idxmax()
            out[key] = {"date": hd.loc[mi], "value": float(w.loc[mi, "H"])}
        else:
            mi = w["L"].idxmin()
            out[key] = {"date": ld.loc[mi], "value": float(w.loc[mi, "L"])}
    return out


def max_drawdown_within(df, d0, d1):
    """[d0, d1] 区间月收盘的最大回撤%（正数）。"""
    w = df[(df["d"] >= d0) & (df["d"] <= d1)]["C"]
    if len(w) < 2:
        return None
    return float(-(w / w.cummax() - 1.0).min() * 100)


# ============================================================================
# 10Y 利率楔形（年度极值通道）
# ============================================================================
def yield_wedge(s):
    """10Y 利率下降楔形拟合（自日频峰值起）。

    方法: 峰值年起取月均值 → 按年取「年最高月均 / 年最低月均」两组锚点
    → 各自线性回归得上/下轨。年度锚点抗月度噪声、可解释性强。
    """
    peak_d, peak_v = s.idxmax(), float(s.max())
    m = s[s.index >= f"{peak_d.year}-01-01"].resample("ME").mean().dropna()
    yr_max = m.groupby(m.index.year).max()
    yr_min = m.groupby(m.index.year).min()
    ph = np.polyfit(yr_max.index.values.astype(float), yr_max.values, 1)
    pl = np.polyfit(yr_min.index.values.astype(float), yr_min.values, 1)
    t = m.index[-1]
    xf = t.year + (t.month - 0.5) / 12.0
    cur = float(m.iloc[-1])
    up, lo = float(np.polyval(ph, xf)), float(np.polyval(pl, xf))
    w0 = float(np.polyval(ph, int(yr_max.index[0])) - np.polyval(pl, int(yr_max.index[0])))
    pos = (cur - lo) / (up - lo) * 100.0 if abs(up - lo) > 1e-9 else float("nan")
    return {
        "peak": (peak_d.date(), peak_v),
        "months_since_peak": months_between(peak_d.date(), t.date()),
        "yr_max": yr_max, "yr_min": yr_min, "m": m,
        "ph": ph, "pl": pl, "cur": cur, "up": up, "lo": lo,
        "pos": pos, "width0": w0, "width1": up - lo,
        "hist_low": (float(s.min()), s.idxmin().date()),
    }


# ============================================================================
# 单指数推演引擎
# ============================================================================
def project_index(df, profile):
    """单指数浪形提取 + 同构推演 → dict（枢轴/浪段/目标/判定/支撑参照）。"""
    m, hd, ld = to_monthly(df)
    piv = find_pivots(m, hd, ld, profile["windows"])
    if not all(k in piv for k in ("w1_low", "w1_top", "w2_low")):
        raise RuntimeError("浪形枢轴不全（窗口需覆盖 w1_low/w1_top/w2_low）")

    w1_low, w1_top, w2_low = piv["w1_low"], piv["w1_top"], piv["w2_low"]
    w2 = df[df["d"] >= w2_low["date"]]
    i_top = w2["H"].idxmax()
    last = w2.iloc[-1]
    w2_top = {"date": w2.loc[i_top, "d"], "value": float(w2.loc[i_top, "H"])}
    now = {"date": last["d"], "value": float(last["C"])}

    # ── 浪段与同构目标 ──
    w1 = {"months": months_between(w1_low["date"], w1_top["date"]),
          "chg": (w1_top["value"] / w1_low["value"] - 1) * 100}
    w2seg = {"months": months_between(w2_low["date"], now["date"]),
             "chg": (now["value"] / w2_low["value"] - 1) * 100}
    t_struct = add_months(w2_low["date"], w1["months"])   # 同构时长目标
    t_clock = add_months(w1_low["date"], JAPAN["b_months"])  # 日本B浪时钟
    p_struct = w2_low["value"] * (1 + w1["chg"] / 100)    # 同构涨幅目标
    space_done = w2seg["chg"] / w1["chg"] * 100 if w1["chg"] else float("nan")
    top_dd = (now["value"] / w2_top["value"] - 1) * 100   # 现价距浪2顶回撤

    # ── 终结判定 ──
    dd_w1 = max_drawdown_within(df, w1_low["date"], w1_top["date"]) or 0.0
    th_dd = max(15.0, dd_w1)
    broken = top_dd <= -th_dd
    closed_min = now["date"] >= min(t_struct, t_clock)
    closed_max = now["date"] >= max(t_struct, t_clock)
    if closed_max and broken:
        verdict = ("ended", "浪2 已终结")
    elif closed_min and broken:
        verdict = ("likely_ended", "浪2 大概率已终结")
    elif closed_max:
        verdict = ("closed", "时间窗已关闭·结构未破")
    elif closed_min:
        # 任一时间目标已到期但回撤未破位: 时间维度开始关闭, 处于警戒段
        verdict = ("warn", "时间窗临期·回撤贴线")
    elif broken:
        verdict = ("broken", "回撤已破位·时间窗未到")
    else:
        verdict = ("alive", "浪2 延续中")

    # ── 终结后参照 ──
    span = w2_top["value"] - w2_low["value"]
    fib = {f"{int(r * 1000) / 10}%": w2_top["value"] - span * r
           for r in (0.382, 0.5, 0.618)}
    c_ref = None
    if "top2015" in piv:  # A浪深度折算日本C浪（科创50 无 2015 前史, 不折算）
        a_drop = w1_low["value"] / piv["top2015"]["value"] - 1
        k = abs(a_drop) / abs(JAPAN["a_drop"])
        c_ref = {"k": k,
                 "target": w2_top["value"] * (1 + k * JAPAN["c_drop"]),
                 "end": add_months(w2_top["date"], JAPAN["c_months"]),
                 "scaled": True}
    else:
        c_ref = {"k": 1.0,
                 "target": w2_top["value"] * (1 + JAPAN["c_drop"]),
                 "end": add_months(w2_top["date"], JAPAN["c_months"]),
                 "scaled": False}

    return {
        "m": m, "piv": piv, "w2_top": w2_top, "now": now,
        "w1": w1, "w2seg": w2seg, "dd_w1": dd_w1, "th_dd": th_dd,
        "t_struct": t_struct, "t_clock": t_clock, "p_struct": p_struct,
        "space_done": space_done, "top_dd": top_dd,
        "broken": broken, "closed_min": closed_min, "closed_max": closed_max,
        "verdict": verdict, "fib": fib, "c_ref": c_ref,
    }


# ============================================================================
# 报告渲染（console 与 markdown 共用同一份行）
# ============================================================================
VERDICT_TITLES = {
    "ended": "浪2 已终结",
    "likely_ended": "浪2 大概率已终结",
    "closed": "时间窗已关闭·结构未破",
    "warn": "时间窗临期·回撤贴线",
    "broken": "回撤已破位·时间窗未到",
    "alive": "浪2 延续中",
}


def _verdict_detail(d):
    """判定依据一行文字。"""
    p = d["piv"]
    parts = [
        f"时间窗：同构 {d['t_struct']:%Y-%m}"
        f"（{'已过' if d['now']['date'] >= d['t_struct'] else '未到'}）、"
        f"时钟 {d['t_clock']:%Y-%m}"
        f"（{'已过' if d['now']['date'] >= d['t_clock'] else '未到'}）",
        f"破位线：距浪2顶回撤 ≥ {d['th_dd']:.1f}%（15% 与浪1内最大回撤 {d['dd_w1']:.1f}% 取大），"
        f"当前 {d['top_dd']:+.1f}%{'（已破）' if d['broken'] else '（未破）'}",
    ]
    return parts


def _index_section(L, profile, d):
    """单指数推演节。"""
    name = profile["name"]
    p = d["piv"]
    w1_low, w1_top, w2_low = p["w1_low"], p["w1_top"], p["w2_low"]
    A = L.append
    lvl, title = d["verdict"]
    A(f"## {name} —— {title}")
    A("")
    A("| 浪段 | 起点 → 终点 | 月数 | 涨跌幅 |")
    A("| --- | --- | --- | --- |")
    A(f"| 浪1 | {w1_low['date']:%Y-%m} {w1_low['value']:.2f} → "
      f"{w1_top['date']:%Y-%m} {w1_top['value']:.2f} | {d['w1']['months']} | {d['w1']['chg']:+.1f}% |")
    A(f"| 浪2（进行中） | {w2_low['date']:%Y-%m} {w2_low['value']:.2f} → "
      f"{d['now']['date']:%Y-%m} {d['now']['value']:.2f} | {d['w2seg']['months']} | {d['w2seg']['chg']:+.1f}% |")
    A(f"| ↳ 浪2顶 | {d['w2_top']['date']:%Y-%m} {d['w2_top']['value']:.2f} "
      f"（现价距其 {d['top_dd']:+.1f}%） | - | - |")
    A("")
    A("**推演目标（论断②同构 + 论断①时钟）**")
    A("")
    gap_p = (d["now"]["value"] / d["p_struct"] - 1) * 100
    A(f"- 时间：同构目标 **{d['t_struct']:%Y-%m}**、时钟目标 **{d['t_clock']:%Y-%m}**"
      f"（浪1底 {w1_low['date']:%Y-%m} + 日本B浪 {JAPAN['b_months']} 个月）")
    A(f"- 空间：同构目标 **{d['p_struct']:.2f}**（浪2底 × (1+浪1涨幅)），"
      f"现价距其 {gap_p:+.1f}%，空间完成度 {d['space_done']:.0f}%（浪2涨幅/浪1涨幅）")
    for t in _verdict_detail(d):
        A(f"- {t}")
    A("")
    A("**后市路径**")
    A("")
    if lvl in ("ended", "likely_ended"):
        A(f"- 判定：浪2 于 **{d['w2_top']['date']:%Y-%m}**（{d['w2_top']['value']:.2f}）见顶"
          f"{'（高置信）' if lvl == 'ended' else '（时钟已过 + 回撤破位）'}，"
          f"当前处于 B 浪收尾 / C 浪酝酿段。反抽不创新高视为逃命线。")
        A("- 结构支撑（浪2 幅度斐波那契回撤）：" +
          "、".join(f"{k} = {v:.2f}" for k, v in d["fib"].items()))
        c = d["c_ref"]
        tag = f"按A浪深度/日本折算（系数 {c['k']:.2f}）" if c["scaled"] else "日本全额未折算（无2015前史）"
        A(f"- 极端边界：日本C浪参照 {JAPAN['c_drop'] * 100:.1f}%/{JAPAN['c_months']} 个月，{tag}"
          f"→ **{c['target']:.2f}（{c['end']:%Y-%m} 前后）**。日本史实、文章未涉及，仅作 3 年尺度框架边界，非预测。")
    elif lvl == "closed":
        A(f"- 判定：两个时间目标均已到期但回撤未破位（{d['top_dd']:+.1f}%）——浪2 处于"
          f"「超时赶顶」段：要么冲击同构目标 {d['p_struct']:.2f} 后终结，要么以时间换空间直接回落。")
        A(f"- 确认信号：回撤加深至 {d['th_dd']:.1f}%（≈ {d['w2_top']['value'] * (1 - d['th_dd'] / 100):.2f}）"
          f"即判定终结；未破位前按冲顶窗口对待。")
        A("- 结构支撑（浪2 幅度斐波那契回撤）：" +
          "、".join(f"{k} = {v:.2f}" for k, v in d["fib"].items()))
    elif lvl == "warn":
        expired = [f"{'同构' if d['t_struct'] <= d['t_clock'] else '时钟'}目标 "
                   f"{min(d['t_struct'], d['t_clock']):%Y-%m}"]
        line_dd = d["w2_top"]["value"] * (1 - d["th_dd"] / 100)
        A(f"- 判定：{'、'.join(expired)} 已到期、回撤 {d['top_dd']:+.1f}% 距破位线 "
          f"{d['th_dd']:.1f}% 仅 {d['th_dd'] + d['top_dd']:.1f}pp——时间维度开始关闭，"
          f"浪2 处于终结前的警戒段。")
        A(f"- 多空分水岭：跌破 **{line_dd:.2f}**（浪2顶 × (1-{d['th_dd']:.1f}%)）确认终结；"
          f"反抽创新高则时间目标顺延至 {max(d['t_struct'], d['t_clock']):%Y-%m}。")
        if d["space_done"] >= 100:
            A(f"- 空间维度：同构目标 {d['p_struct']:.2f} 早已被超越（完成度 {d['space_done']:.0f}%），"
              "后市由时间目标与回撤阈值主导，空间目标失效。")
        A("- 结构支撑（浪2 幅度斐波那契回撤）：" +
          "、".join(f"{k} = {v:.2f}" for k, v in d["fib"].items()))
    elif lvl == "broken":
        A(f"- 判定：回撤 {d['top_dd']:+.1f}% 已破位但时间窗未到——同构关系受损，"
          f"需警惕浪2 提前终结（强波动结构，对照创业板 2021-12 顶形态）。")
        A("- 结构支撑（浪2 幅度斐波那契回撤）：" +
          "、".join(f"{k} = {v:.2f}" for k, v in d["fib"].items()))
    else:
        remain = months_between(d["now"]["date"], min(d["t_struct"], d["t_clock"]))
        A(f"- 判定：浪2 延续。距离最近的时间目标还有 {remain} 个月，"
          f"空间完成度 {d['space_done']:.0f}%。")
        if d["space_done"] < 100:
            A(f"- 冲顶路径：向同构目标 {d['p_struct']:.2f}（{gap_p:+.1f}%）推进，"
              f"于时间窗内（{min(d['t_struct'], d['t_clock']):%Y-%m} 前后）见顶。")
        else:
            A(f"- 空间已超同构目标（完成度 {d['space_done']:.0f}%），后市由时间目标主导："
              f"{min(d['t_struct'], d['t_clock']):%Y-%m} 前后见顶概率大。")
    A("")
    # 指数特记
    if profile["code"] == "sz399006":
        ath = d["m"]["H"].max()
        if d["w2_top"]["value"] >= ath:
            A("> 特记：浪2顶已创该指数历史新高（突破 2015 大顶）——在「熊市反弹大结构」"
              "论断下属超强反弹；若大结构成立，其终结后的回落空间与波动率亦为三指数之最。")
            A("")
    if profile["code"] == "sh000688":
        A("> 特记：科创50 无 2015 前史（2019-12 基日），B 浪时钟自 2020-01 起算，"
          "为三指数中唯一未到期的时钟；其浪2 弹性远超同构目标，结构参考意义弱于时钟意义。")
        A("")


def build_report(results, wedge, yield_err):
    """生成报告行列表。results: [{profile, d}]。"""
    L = []
    A = L.append
    today = date.today().strftime("%Y-%m-%d")
    names = "/".join(p["name"] for p, _ in results)
    A(f"# A股后市推演：{names}（{today}）")
    A("")
    A("> 前提论断（《浅析A股大结构》，趋势之美07 2026-08-20）：① A股 2015 大顶后处于熊市")
    A("> 反弹大结构（大B浪），日本同构 B 浪全长 90 个月；② 2019-2021（浪1）与 2024-2026")
    A("> （浪2）为 B 浪中两个同构反弹子浪；③ 10Y 国债利率长期下降楔形为经济周期锚。")
    A("> 本报告依据论断做规则化推演，全部点位与日期由当期真实数据实测。")
    A("")

    # ── 一、大势前提 ──
    A("## 一、大势前提：B 浪时钟与利率锚")
    A("")
    for p, d in results:
        A(f"- {p['name']}：浪1底 {d['piv']['w1_low']['date']:%Y-%m} + 90 个月 = "
          f"**{d['t_clock']:%Y-%m}**（{'已过' if d['now']['date'] >= d['t_clock'] else '未到'}，"
          f"距今 {abs(months_between(d['now']['date'], d['t_clock']))} 个月）")
    if wedge is not None:
        below = wedge["cur"] < wedge["lo"]
        A(f"- 10Y 利率：现值 {wedge['cur']:.2f}%，"
          + (f"**已跌破下轨 {wedge['lo']:.2f}%**" if below
             else f"通道内 {wedge['pos']:.0f}% 分位（下轨 {wedge['lo']:.2f}%）")
          + f"；楔形自 {wedge['peak'][0]} 峰值（{wedge['peak'][1]:.2f}%）收敛 "
          f"{100 * (1 - wedge['width1'] / wedge['width0']):.0f}%")
        if below:
            A(f"  - 论断③推论：日本 1998 破位后 B 浪仍延续约 {JAPAN['post_break_b_months']} 个月至 2000-03 顶；"
              f"中国若以 {wedge['hist_low'][1]}（历史低 {wedge['hist_low'][0]:.2f}%，首次贴破下轨）起算，"
              f"+{JAPAN['post_break_b_months']} 个月 ≈ "
              f"**{add_months(pd.Timestamp(wedge['hist_low'][1]), JAPAN['post_break_b_months']):%Y-%m}**——"
              "利率维度给出的 B 浪收尾窗口。")
    else:
        A(f"- 10Y 利率：数据不可达（{yield_err}），跳过利率锚推论")
    A("")

    # ── 二~四、各指数 ──
    for p, d in results:
        _index_section(L, p, d)

    # ── 五、对照与结论 ──
    A("## 对照与综合结论")
    A("")
    A("| 指数 | 浪2 | 距顶回撤 | 空间完成度 | 时间目标（同构/时钟） | 判定 |")
    A("| --- | --- | --- | --- | --- | --- |")
    for p, d in results:
        A(f"| {p['name']} | {d['w2seg']['months']} 个月 {d['w2seg']['chg']:+.1f}% "
          f"| {d['top_dd']:+.1f}% | {d['space_done']:.0f}% "
          f"| {d['t_struct']:%Y-%m} / {d['t_clock']:%Y-%m} | {VERDICT_TITLES[d['verdict'][0]]} |")
    A("")
    lvls = [d["verdict"][0] for _, d in results]
    n_ended = sum(1 for v in lvls if v in ("ended", "likely_ended"))
    n_warn = sum(1 for v in lvls if v in ("warn", "closed"))
    if n_ended >= 2:
        A(f"- 格局：三指数中 {n_ended} 个已入终结判定，反弹大结构进入收尾段的证据占优；"
          "尚未破位者（若有）以「赶顶/反抽」对待，创新高仅视为延长浪。")
    elif n_ended == 1:
        A("- 格局：结构分化——一指数入终结判定、其余未破位；按木桶原则以最弱指数为风向标，"
          "其余指数的时间目标仍有效但胜率下调。")
    elif n_warn >= 2:
        A(f"- 格局：{n_warn} 个指数时间窗已关闭或临期，无破位确认——反弹大结构处于"
          "「赶顶/派发」重叠段：时间目标不再提供上行空间，仅剩反抽动能；"
          "各指数破位线（见上文）为终结确认的分水岭。")
    elif n_warn == 1:
        A("- 格局：一指数时间窗临期、其余延续；临期指数的回撤若先于时间目标破位，"
          "按信号传导对待（高弹性板块先行）。")
    else:
        A("- 格局：三指数浪2 结构均未终结，反弹大结构按论断②延续至各自时间目标。")
    A("- 风险提示：本推演完全依赖「熊市反弹大结构」论断成立；若 2015 顶后的结构定性错误"
      "（如属新一轮长期牛市），全部时间/空间目标失效。浪形锚点为规则化窗口极值，"
      "存在 ±1 个月的定位误差。不构成投资建议。")
    A("")
    return L


# ============================================================================
# 图表（2×2: 三指数浪形推演 + 利率楔形）
# ============================================================================
def render_chart(path, results, wedge):
    """渲染 png: 三指数月线浪形+目标位, 右下利率楔形。"""
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt
    plt.rcParams["font.sans-serif"] = ["Noto Sans CJK JP", "AR PL UMing CN", "sans-serif"]
    plt.rcParams["axes.unicode_minus"] = False

    fig, axes = plt.subplots(2, 2, figsize=(15, 10))
    for ax, (profile, d) in zip(axes.flat[:3], results):
        m = d["m"]
        mm = m[m.index >= pd.Timestamp(profile["chart_from"])]
        x = pd.DatetimeIndex(mm.index)
        ax.plot(x, mm["C"], lw=1.3, color="#1a6faf")
        ax.set_title(f"{profile['name']} —— {VERDICT_TITLES[d['verdict'][0]]}",
                     fontsize=12, color="crimson")
        ax.grid(alpha=0.3)
        marks = [("w1_low", "浪1底", -32), ("w1_top", "浪1顶", 14),
                 ("w2_low", "浪2底", -32)]
        for key, label, dy in marks:
            pv = d["piv"][key]
            dt = pd.Timestamp(pv["date"])
            ax.scatter([dt], [pv["value"]], s=28, color="crimson", zorder=5)
            ax.annotate(f"{label}\n{pv['date']:%Y-%m} {pv['value']:.0f}",
                        (dt, pv["value"]), textcoords="offset points",
                        xytext=(0, dy), ha="center", fontsize=8, color="crimson")
        dt = pd.Timestamp(d["w2_top"]["date"])
        ax.scatter([dt], [d["w2_top"]["value"]], s=30, color="darkorange", zorder=5)
        ax.annotate(f"浪2顶\n{d['w2_top']['date']:%Y-%m} {d['w2_top']['value']:.0f}",
                    (dt, d["w2_top"]["value"]), textcoords="offset points",
                    xytext=(0, 14), ha="center", fontsize=8, color="darkorange")
        # fib 支撑 + 同构空间目标（文字锚点取图内右侧 78% 处, 防溢出）
        for k, v in d["fib"].items():
            ax.axhline(v, ls=":", lw=0.8, color="#8e44ad", alpha=0.7)
        ax.axhline(d["p_struct"], ls="--", lw=1.0, color="#e67e22", alpha=0.9)
        x_txt = x[int(len(x) * 0.78)]
        ax.text(x_txt, d["p_struct"], f"同构目标 {d['p_struct']:.0f}",
                fontsize=7.5, color="#e67e22", va="bottom", ha="right")
        if d["fib"]:
            fk = list(d["fib"].keys())
            ax.text(x_txt, d["fib"][fk[-1]], f"fib 61.8%={d['fib'][fk[-1]]:.0f}",
                    fontsize=7, color="#8e44ad", va="bottom", ha="right")

    ax2 = axes.flat[3]
    if wedge is None:
        ax2.text(0.5, 0.5, "10Y 利率数据不可达", ha="center", va="center",
                 transform=ax2.transAxes)
        ax2.set_xticks([]); ax2.set_yticks([])
    else:
        mm_y = wedge["m"]
        ax2.plot(mm_y.index, mm_y.values, lw=0.9, color="#666666", label="10Y 月均")
        years = wedge["yr_max"].index.values.astype(float)
        ax2.scatter(years, wedge["yr_max"].values, s=18, color="#c0392b",
                    zorder=5, label="年最高月均")
        ax2.scatter(years, wedge["yr_min"].values, s=18, color="#2980b9",
                    zorder=5, label="年最低月均")
        t = mm_y.index[-1]
        xf = t.year + (t.month - 0.5) / 12.0
        xx = np.linspace(years.min(), xf, 60)
        ax2.plot(xx, np.polyval(wedge["ph"], xx), ls="--", lw=1.0,
                 color="#c0392b", label="上轨")
        ax2.plot(xx, np.polyval(wedge["pl"], xx), ls="--", lw=1.0,
                 color="#2980b9", label="下轨")
        below = wedge["cur"] < wedge["lo"]
        ax2.scatter([xf], [wedge["cur"]], s=45, marker="*",
                    color="crimson" if below else "seagreen", zorder=6,
                    label=f"当前 {wedge['cur']:.2f}%")
        ax2.set_title("中国 10Y 国债收益率下降楔形（论断③锚）", fontsize=11)
        ax2.legend(fontsize=8, ncol=3, loc="upper right")
        ax2.grid(alpha=0.3)
        ax2.set_ylabel("%")

    fig.suptitle("A股后市推演 —— 熊市反弹大结构（论断①②③）", fontsize=14)
    fig.tight_layout(rect=(0, 0, 1, 0.97))
    fig.savefig(path, dpi=150)
    plt.close(fig)


# ============================================================================
# main
# ============================================================================
def main(argv=None):
    ap = argparse.ArgumentParser(
        description="A股后市推演——依据《浅析A股大结构》论断的上证/创业板/科创50 走势分析")
    ap.add_argument("--no-chart", action="store_true", help="不渲染 png 图")
    args = ap.parse_args(sys.argv[1:] if argv is None else argv)

    # ── 指数数据（TDengine） ──
    results = []
    for profile in INDEX_PROFILES:
        try:
            df = fetch_index_daily(profile["code"])
            results.append((profile, project_index(df, profile)))
        except Exception as e:
            print(f"[warn] {profile['code']} {profile['name']} 数据缺失: {e}")
    if not results:
        sys.exit("全部指数数据缺失，退出")

    # ── 10Y 利率（东财） ──
    print("获取 10Y 国债收益率（东方财富）...")
    try:
        s, yerr = fetch_yield10y()
    except Exception as e:
        s, yerr = None, str(e)
    wedge = yield_wedge(s) if s is not None else None
    if wedge is None:
        print(f"[warn] 利率数据不可达: {yerr}")

    # ── 报告 ──
    lines = build_report(results, wedge, yerr)
    text = "\n".join(lines)
    print(text)

    out_dir = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
                           "output", "market-struct")
    os.makedirs(out_dir, exist_ok=True)
    md_path = os.path.join(out_dir, f"market-struct-{date.today():%Y%m%d}.md")
    with open(md_path, "w", encoding="utf-8") as f:
        f.write(text + "\n")
    print(f"\n✅ 报告: {md_path}")

    # ── 图 ──
    if not args.no_chart:
        try:
            png_path = md_path.replace(".md", ".png")
            render_chart(png_path, results, wedge)
            print(f"✅ 图表: {png_path}")
        except Exception as e:
            print(f"[warn] 图表渲染失败（不影响报告）: {e}")


if __name__ == "__main__":
    main()
