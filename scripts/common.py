"""选股脚本共享工具 (scalper/leader/smmd)。

提取自三脚本逐字重复的代码: 代码解析、自选股读取、全市场标的池。
"""

import os
import re
import sys
import unicodedata

import numpy as np
import pandas as pd
import taosws

# 自选股板块文件 (通达信 zxg.blk)。环境变量 TDX_ZXG_BLK 可覆盖。
ZXG_PATH = os.environ.get("TDX_ZXG_BLK", "/home/li/.local/share/tdxcfv/drive_c/tc/T0002/blocknew/zxg.blk")

# 项目根 output/（scripts/ 上级目录锚定，不随运行 cwd 漂移）
OUTPUT_DIR = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "output")

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
         / (pre_close*(1+songzhuangu+peigu)); 累乘即最终因子 (不归一, 最新 bar 天然=1);
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
    # 注意: 不做「末尾归一」。forward-asof 下 factor(bar)=∏(date>bar 事件的 ef)，
    # 最新 bar 天然=1；若除以最新事件的 ef（曾对照上游 adjust.py 的归一）会把
    # 最新事件的复权效果从所有 bar 约掉——单事件股票 qfq 完全失效（实测 2026-08）。
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

# ============================================================================
# 选股共享工具（并入自 find_diverse_common.py）
# ============================================================================
TDENGINE_URL = os.environ.get("TDENGINE_URL", "taosws://root:taosdata@localhost:6041/tdx")

SECTOR_INDICES = {
    "sh000688": "科创", "sz399006": "创业板", "sz399001": "深证成指",
    "sh000001": "上证", "sh000300": "沪深300", "sh000905": "中证500",
    "sh000852": "中证1000", "bj899050": "北证50",
}

CODE_SECTOR_MAP = [
    (("68",), "sh000688"),
    (("30",), "sz399006"),
    (("00", "60"), "sh000300"),
    (("43", "83", "87", "92"), "bj899050"),
]


def connect():
    conn = taosws.connect(TDENGINE_URL)
    conn.query("USE tdx")
    return conn


def fetch_kline(conn, market, code, cycle="1d", days=400, min_rows=2):
    """查单只 K 线。返回 DataFrame 或 None。"""
    start = (pd.Timestamp.now() - pd.Timedelta(days=days)).strftime("%Y-%m-%d")
    r = conn.query(
        f"SELECT ts,open,high,low,close,volume FROM k_{market}{code}_{cycle} "
        f"WHERE ts >= '{start}' ORDER BY ts"
    )
    if len(r) < min_rows:
        return None
    return pd.DataFrame(list(r), columns=["ts", "O", "H", "L", "C", "V"])


def batch_fetch_klines(conn, pool, days=400, min_rows=252):
    """批量查全市场 1d → {(market, code): DataFrame}。"""
    queries = []
    for m, c in pool:
        queries.append(
            f"SELECT ts,open,high,low,close,volume,'{m}{c}' as code FROM k_{m}{c}_1d"
        )
    if not queries:
        return {}
    sql = " UNION ALL ".join(queries) + " ORDER BY code, ts"
    r = conn.query(sql)
    out = {}
    cur_m, cur_c, buf = None, None, []
    for row in r:
        mc = row[6]
        m, c = mc[:2], mc[2:]
        if (m, c) != (cur_m, cur_c):
            if cur_m and len(buf) >= min_rows:
                out[(cur_m, cur_c)] = pd.DataFrame(buf, columns=["ts", "O", "H", "L", "C", "V"])
            cur_m, cur_c, buf = m, c, []
        buf.append(row[:6])
    if cur_m and len(buf) >= min_rows:
        out[(cur_m, cur_c)] = pd.DataFrame(buf, columns=["ts", "O", "H", "L", "C", "V"])
    return out


def load_stock_names(conn):
    try:
        return {f"{m}{c}": n for m, c, n in conn.query(
            "SELECT market, code, name FROM tdx.stock_name")}
    except Exception:
        return {}


def add_indicators(df):
    """计算技术指标列。"""
    df = df.copy()
    df["MA5"] = df["C"].rolling(5).mean()
    df["MA20"] = df["C"].rolling(20).mean()
    df["MA60"] = df["C"].rolling(60).mean()
    df["MA120"] = df["C"].rolling(120).mean()
    df["MA250"] = df["C"].rolling(250).mean()
    df["ret_250"] = df["C"].pct_change(250) * 100
    df["ret_120"] = df["C"].pct_change(120) * 100
    df["high_250"] = df["C"].rolling(250).max()
    df["near_high"] = df["C"] / df["high_250"]
    delta = df["C"].diff()
    gain = delta.clip(lower=0).rolling(14).mean()
    loss = (-delta.clip(upper=0)).rolling(14).mean()
    rs = gain / loss.replace(0, np.nan)
    df["RSI14"] = 100 - 100 / (1 + rs)
    df["RSI_bull"] = (df["RSI14"] > 50).astype(int)
    # CCI
    tp = (df["H"] + df["L"] + df["C"]) / 3
    sma = tp.rolling(20).mean()
    mad = tp.rolling(20).apply(lambda x: np.abs(x - x.mean()).mean(), raw=True)
    df["CCI"] = (tp - sma) / (0.015 * mad)
    return df


def add_dmi(df, period=14):
    """ADX / +DI / -DI。"""
    up = df["H"].diff()
    down = -df["L"].diff()
    plus_dm = np.where((up > down) & (up > 0), up, 0.0)
    minus_dm = np.where((down > up) & (down > 0), down, 0.0)
    tr = np.maximum(df["H"] - df["L"],
                   np.maximum(abs(df["H"] - df["C"].shift()),
                              abs(df["L"] - df["C"].shift())))
    atr = pd.Series(tr).rolling(period).mean()
    plus_di = 100 * pd.Series(plus_dm).rolling(period).mean() / atr
    minus_di = 100 * pd.Series(minus_dm).rolling(period).mean() / atr
    dx = 100 * abs(plus_di - minus_di) / (plus_di + minus_di).replace(0, np.nan)
    df["ADX"] = dx.rolling(period).mean()
    df["plus_DI"] = plus_di
    df["minus_DI"] = minus_di
    return df


def _compute(m, c, df, adj_events, need_ind=True):
    """单只预处理: 复权 + 指标。"""
    if df is None:
        return None
    df = apply_qfq(df, adj_events)
    if need_ind:
        df = add_indicators(df)
        df = add_dmi(df)
    return (m, c, df)


def to_weekly(df):
    """日线 → 周线。"""
    df = df.copy()
    df["ts"] = pd.to_datetime(df["ts"])
    df = df.set_index("ts").resample("W").agg({
        "O": "first", "H": "max", "L": "min", "C": "last", "V": "sum"
    }).dropna().reset_index()
    return df


def thread_conn():
    conn = taosws.connect(TDENGINE_URL)
    conn.query("USE tdx")
    return conn


def sector_momentum(conn, days=20):
    """板块指数近 N 日涨幅。"""
    out = {}
    start = (pd.Timestamp.now() - pd.Timedelta(days=days * 2)).strftime("%Y-%m-%d")
    for sec in SECTOR_INDICES:
        try:
            r = conn.query(
                f"SELECT ts,close FROM k_{sec}_1d WHERE ts >= '{start}' ORDER BY ts"
            )
            if len(r) >= 2:
                out[sec] = (r[-1][1] / r[0][1] - 1) * 100
        except Exception:
            pass
    return out


def get_sector_for_code(code):
    for prefixes, sector in CODE_SECTOR_MAP:
        if code[:2] in prefixes:
            return sector
    return "sh000300"


def resample_intraday(df, tf):
    """5m → 目标周期。"""
    df = df.copy()
    df["ts"] = pd.to_datetime(df["ts"])
    df = df.set_index("ts").resample(tf).agg({
        "O": "first", "H": "max", "L": "min", "C": "last", "V": "sum"
    }).dropna().reset_index()
    return df


def _pivot_lows(cv, k=3, sep=4):
    """波谷位置列表。"""
    n = len(cv)
    piv = []
    for i in range(k, n - k):
        if all(cv[i] <= cv[i - j] for j in range(1, k + 1)) and \
           all(cv[i] <= cv[i + j] for j in range(1, k + 1)):
            if not piv or i - piv[-1] >= sep:
                piv.append(i)
    return piv
