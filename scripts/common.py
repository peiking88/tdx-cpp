"""选股脚本共享工具 (scalper/leader/smmd)。

提取自三脚本逐字重复的代码: 代码解析、自选股读取、全市场标的池。
"""

import os
import re
import sys
import unicodedata

import pandas as pd

# 自选股板块文件 (通达信 zxg.blk)。环境变量 TDX_ZXG_BLK 可覆盖。
ZXG_PATH = os.environ.get("TDX_ZXG_BLK", "/home/li/.local/share/tdxcfv/drive_c/tc/T0002/blocknew/zxg.blk")

# ---------------------------------------------------------------------------
# 表格渲染: 显示宽度感知对齐 (CJK 全角计 2, ASCII 计 1)
# ---------------------------------------------------------------------------
_ANSI_RE = re.compile(r"\x1b\[[0-9;]*m")


def disp_w(s):
    """显示宽度: 剥离 ANSI 后 CJK 全角字符计 2, ASCII 计 1。"""
    return sum(2 if unicodedata.east_asian_width(ch) in ("W", "F") else 1
               for ch in _ANSI_RE.sub("", s))


def pad(s, width, align="<"):
    """按显示宽度对齐填充 (修复中文表头/名称列错位), 保留 ANSI 颜色。"""
    n = width - disp_w(s)
    if n <= 0:
        return s
    return (" " * n + s) if align == ">" else (s + " " * n)


def parse_code(code):
    """解析股票代码 → (market, code)。支持 sh/sz/bj 前缀或裸代码推断。"""
    code = code.strip().lower()
    if code[:2] in ("sh", "sz", "bj"):
        return code[:2], code[2:]
    if code.startswith(("60", "68", "99")):
        return "sh", code
    if code.startswith(("00", "30", "39")):
        return "sz", code
    if code.startswith(("8", "4", "92")):  # 北交: 83/87/43/920
        return "bj", code
    return "sh", code


def zxg_codes(path=ZXG_PATH):
    """读自选股板块文件 → ['sh600000', ...]。每行 '前缀(1=sh/0=sz)+6位代码'。"""
    codes = []
    try:
        with open(path) as f:
            for line in f:
                line = line.strip()
                if len(line) >= 7 and line[0] in "01":
                    codes.append(("sh" if line[0] == "1" else "sz") + line[1:7])
    except FileNotFoundError:
        pass
    return codes


def all_mainboard_codes(conn):
    """全量 A 股个股 (SH/SZ/BJ, 排除指数/基金/B股/债券)。

    必须用 (market, code) 联合过滤: stock_name 表混入了大量 sh000xxx / sz399xxx
    指数 (如 sh000300 沪深300、sh000027 180运输)。若仅按 code 前缀 '00%' 过滤,
    会把这些指数误当个股选入, 且与深市同名代码 (sz000027 深圳能源) 冲突。
    沪市个股仅 60/68 开头, 深市 00/30, 北交所 43/83/87/920。
    """
    try:
        r = conn.query(
            "SELECT code, market FROM tdx.stock_name WHERE "
            "(market='sh' AND (code LIKE '60%' OR code LIKE '68%'))"      # 沪市主板/科创板
            " OR (market='sz' AND (code LIKE '00%' OR code LIKE '30%'))"  # 深市主板/创业板
            " OR (market='bj' AND (code LIKE '43%' OR code LIKE '83%' OR code LIKE '87%' OR code LIKE '920%'))"  # 北交所
        )
        return [(m, c) for c, m in r]
    except Exception:
        return []


# ---------------------------------------------------------------------------
# 前复权 (对齐 src/data/adjust.cpp; leader/smmd 共用)
# ---------------------------------------------------------------------------
def _pershare(value):
    """xdxr 字段归一: >=1 视为「每 10 股」口径 (如 10 送 5 存为 5.0), 除 10 还原。
    对齐 src/data/adjust.cpp:29-32 (PerShare)。"""
    return value / 10.0 if value >= 1.0 else value


def batch_fetch_adjust(conn, pool):
    """批量查全市场除权事件 → {(market, code): [(date, fenhong, peigujia, songzhuangu, peigu, category, name), ...]}."""
    try:
        r = conn.query(
            "SELECT market, code, ts, fenhong, peigujia, songzhuangu, peigu, category, name "
            "FROM tdx.adjust"
        )
    except Exception as e:
        sys.stderr.write(f"[warn] 批量查询复权事件失败 (复权将跳过): {e}\n")
        return {}
    want = {(m, c) for m, c in pool}
    by_mc = {}
    for m, code, ts, fh, pgj, sz, pg, cat, nm in r:
        if (m, code) not in want:
            continue
        by_mc.setdefault((m, code), []).append(
            (str(ts)[:10], fh or 0.0, pgj or 0.0, sz or 0.0, pg or 0.0, cat or 0, nm or "")
        )
    return by_mc


def apply_qfq(df, events):
    """对 df 的 OHLC 应用前复权因子 (对齐 src/data/adjust.cpp)。仅 OHLC, vol/amount 不变。

    qfq: 事件按日期降序 (新→旧); event_factor = (pre_close - fenhong + peigujia*peigu)
         / (pre_close*(1+songzhuangu+peigu)); 累乘后末尾归一 (最新日 factor=1);
         forward-asof (取首个 date>kdate 的因子——除权日 bar 不乘自身事件,
         否则序列在除权日保留假跳空)。仅 category in {1,2} 或 name=='除权除息' 计因子。
    无事件或查询失败时原样返回 (退化为未复权)。
    """
    if not events:
        return df
    ev = sorted(events, key=lambda e: e[0], reverse=True)  # 新→旧
    ts_naive = df["ts"].dt.tz_localize(None)  # df["ts"] 带 +08 时区, 统一 tz-naive 比较
    cumulative = 1.0
    fac = []  # (date_str, cumulative)
    for date_str, fh, pgj, sz, pg, cat, nm in ev:
        prev_mask = ts_naive < pd.Timestamp(date_str)
        pre_close = df.loc[prev_mask, "C"].iloc[-1] if prev_mask.any() else 0.0
        if pre_close == 0.0:
            fac.append((date_str, cumulative))
            continue
        fh = _pershare(fh); sz = _pershare(sz); pg = _pershare(pg)
        if cat in (1, 2) or nm == "除权除息":
            num = pre_close - fh + pgj * pg
            den = pre_close * (1.0 + sz + pg)
            ef = 1.0 if (den == 0.0 or num == 0.0) else num / den
        else:
            ef = 1.0
        cumulative *= ef
        fac.append((date_str, cumulative))
    if not fac:
        return df
    fac.sort(key=lambda x: x[0])  # 升序
    latest = fac[-1][1]
    if latest > 0:
        fac = [(d, f / latest) for d, f in fac]  # 末尾归一
    fac_ts = [pd.Timestamp(d) for d, _ in fac]
    fac_val = [f for _, f in fac]

    def factor_for(ts):
        # forward-asof: 找第一个 date > ts 的事件因子。除权日 bar 在事件前 (pre_close)
        # 取值, 已是除权后价格, 不应乘自身事件因子; 否则序列在除权日出现假跳空。
        for d, v in zip(fac_ts, fac_val):
            if d > ts:
                return v
        return 1.0

    fs = ts_naive.map(factor_for)
    out = df.copy()
    for col in ("O", "H", "L", "C"):
        out[col] = out[col] * fs
    return out
