#!/usr/bin/env python3
"""
隔夜套利战法选股 (时空置换) —— 尾盘 14:30 启动循环版本。

照搬 overnight-scalper 六步法则 + 增强信号，差异：
  - 每日 14:30 (可配置) 自动启动，循环运行至 15:00 收盘
  - 每轮刷新表格输出（ANSI 清屏），实时展示命中标的 + 指标
  - 数据源：tdx-cpp 共享内存 + TDengine

六步法则：
  1. 涨幅 3%-5%，主板 (60/00)
  2. 近 N 天有涨停基因
  3. 量比 > 1
  4. 流通市值 < 200 亿
  5. 换手率 5%-10%
  6. 全天 ≥60% 分钟站稳 VWAP 上方

用法:
  python3 scripts/scalper.py                                   # 自选股, 14:30-15:00 循环
  python3 scripts/scalper.py --all                             # 全量主板
  python3 scripts/scalper.py --start 14:25 --end 15:05         # 自定义时段
  python3 scripts/scalper.py --interval 30                     # 刷新间隔 (秒)
  python3 scripts/scalper.py --once                            # 单次运行 (不循环)
  python3 scripts/scalper.py --gain-low 2.5 --gain-high 5.5    # 调参数
"""

import argparse
import bisect
import csv
import json
import mmap
import os
import struct
import sys
import time
from collections import defaultdict
from concurrent.futures import ThreadPoolExecutor, as_completed
from datetime import datetime, timedelta

import pandas as pd
import taosws
from common import (all_mainboard_codes, parse_code, zxg_codes,
                    apply_qfq, batch_fetch_adjust, disp_w, pad)

# ---------------------------------------------------------------------------
# 配置
# ---------------------------------------------------------------------------
TDENGINE_URL = os.environ.get("TDENGINE_URL", "taosws://root:taosdata@localhost:6041")
DEFAULT_SHM = "/dev/shm/tdx_quotes.shm"
WP_PATH = "/home/li/.local/share/tdxcfv/drive_c/tc/T0002/blocknew/WP.blk"

# 验证 (verify 子命令): 资金 10 万, 买入=前日 14:30-15:00 算术均价, 卖出=当日 9:30-10:00 算术均价
VERIFY_CAPITAL = 100000.0
VERIFY_CSV = "output/scalper/verify.csv"
VERIFY_FIELDS = ["verify_date", "code", "name", "buy_date", "buy_price", "buy_bars",
                 "sell_date", "sell_price", "sell_bars", "shares", "cost", "income",
                 "pnl", "pnl_pct"]

# TDengine 持久化超级表 (对齐项目惯例: kline/finance 的 STable + 子表 + TAGS 模式)
PICK_DDL = (
    "CREATE STABLE IF NOT EXISTS scalper_pick ("
    "ts TIMESTAMP, price DOUBLE, gain_pct DOUBLE, vol_ratio DOUBLE, "
    "turnover_pct DOUBLE, cap_yi DOUBLE, vwap_above_ratio DOUBLE, "
    "signal_score DOUBLE, signal_hit INT, signal_total INT, "
    "enhancement_json VARCHAR(4096)) "
    "TAGS (code VARCHAR(10), market VARCHAR(2), name VARCHAR(64))")
VERIFY_DDL = (
    "CREATE STABLE IF NOT EXISTS scalper_verify ("
    "ts TIMESTAMP, buy_date VARCHAR(10), buy_price DOUBLE, buy_bars INT, "
    "sell_price DOUBLE, sell_bars INT, shares DOUBLE, cost DOUBLE, "
    "income DOUBLE, pnl DOUBLE, pnl_pct DOUBLE) "
    "TAGS (code VARCHAR(10), market VARCHAR(2), name VARCHAR(64))")

# shm 二进制布局 (与 include/tdx/shm/{segment,snapshot,payload}.hpp 一致)
POD_FMT = "< q 27d"  # datetime(i64) + 27×double = 224B
assert struct.calcsize(POD_FMT) == 224
# POD 字段索引 (与 include/tdx/shm/payload.hpp 字段顺序一致, 避免裸数字易错)
POD_TS, POD_PRICE, POD_PRE_CLOSE, POD_OPEN = 0, 1, 2, 3
POD_HIGH, POD_LOW, POD_VOLUME, POD_AMOUNT = 4, 5, 6, 7
SLOT_SIZE = 256
HDR_OFF_MAGIC = 0
HDR_OFF_VERSION = 8
HDR_OFF_SNAP_SLOTS = 56
HDR_OFF_SNAP_SLOT_SIZE = 60
HDR_OFF_SNAP_OFF = 64
SLOT_OFF_SEQ = 0
SLOT_OFF_CODE = 8
SLOT_OFF_POD = 16
SLOT_OFF_FLAGS = 240

MAIN_BOARD_PREFIXES = ("60", "00")
LIMIT_UP_PCT = 9.8

DEFAULTS = {
    "gain_low": 3.0,
    "gain_high": 5.0,
    "lookback_days": 30,
    "vol_ratio": 1.0,
    "cap_max": 200.0,
    "turnover_low": 5.0,
    "turnover_high": 10.0,
    "turnover_2d_max": 15.0,
    "vwap_above_ratio": 0.6,
    "tail_mom_lookback": 20,
    "tail_mom_start": 1331,
    "vol_high_lookback": 10,
    "rps_periods": [50, 120, 250],
    "ma_periods": [90, 100, 120],
    "ma_align_periods": [5, 10, 20, 60],
    "breakout_close_ratio": 0.85,
    "vol_high_mult": 1.0,
    "rps_baseline_days": 400,
}


# ---------------------------------------------------------------------------
# 共享内存读取 (只读 seqlock)
# ---------------------------------------------------------------------------
class ShmReader:
    def __init__(self, path):
        self.path = path
        self._mm = None
        self._fd = None
        self._slots = 0
        self._snap_off = 0

    def open(self):
        try:
            self._fd = os.open(self.path, os.O_RDONLY)
        except OSError:
            return False
        st = os.fstat(self._fd)
        if st.st_size < 1024:
            os.close(self._fd)
            return False
        self._mm = mmap.mmap(self._fd, 0, access=mmap.ACCESS_READ)
        if self._mm[HDR_OFF_MAGIC:HDR_OFF_MAGIC + 8] != b"TDXSHM\0\0":
            return self._fail()
        version = struct.unpack_from("< I", self._mm, HDR_OFF_VERSION)[0]
        if version != 1:
            return self._fail()
        snap_slots, _ = struct.unpack_from("< II", self._mm, HDR_OFF_SNAP_SLOTS)
        snap_off = struct.unpack_from("< Q", self._mm, HDR_OFF_SNAP_OFF)[0]
        if snap_off + snap_slots * SLOT_SIZE > st.st_size:
            return self._fail()
        self._slots, self._snap_off = snap_slots, snap_off
        return True

    def _fail(self):
        self.close()
        return False

    def close(self):
        if self._mm is not None:
            self._mm.close()
        if self._fd is not None:
            os.close(self._fd)
        self._mm = self._fd = None

    def get(self, code):
        bs = code.encode("ascii") if isinstance(code, str) else code
        if len(bs) > 7 or not bs:
            return None
        mask = self._slots - 1
        h = 2166136261
        for c in bs:
            h = ((h ^ c) * 16777619) & 0xFFFFFFFF
        idx0 = h & mask
        mm = self._mm
        snap_off = self._snap_off
        for _retries in range(8):
            idx = idx0
            for _probe in range(self._slots):
                off = snap_off + idx * SLOT_SIZE
                s1 = struct.unpack_from("< Q", mm, off + SLOT_OFF_SEQ)[0]
                if s1 & 1:
                    break
                block_c = mm[off + SLOT_OFF_CODE:off + SLOT_OFF_CODE + 8]
                pod = struct.unpack_from(POD_FMT, mm, off + SLOT_OFF_POD)
                s2 = struct.unpack_from("< Q", mm, off + SLOT_OFF_SEQ)[0]
                if s1 != s2:
                    break
                if block_c[0] == 0:
                    return None
                end = block_c.find(b"\x00")
                if end < 0:
                    end = 8
                if end == len(bs) and block_c[:end] == bs:
                    return pod
                idx = (idx + 1) & mask
        return None

    def all_quotes(self):
        out = {}
        mm = self._mm
        snap_off = self._snap_off
        for i in range(self._slots):
            off = snap_off + i * SLOT_SIZE
            s1 = struct.unpack_from("< Q", mm, off + SLOT_OFF_SEQ)[0]
            if s1 & 1:
                continue
            block_c = mm[off + SLOT_OFF_CODE:off + SLOT_OFF_CODE + 8]
            s2 = struct.unpack_from("< Q", mm, off + SLOT_OFF_SEQ)[0]
            if s1 != s2 or block_c[0] == 0:
                continue
            end = block_c.find(b"\x00")
            if end <= 0:
                continue
            code = block_c[:end].decode("ascii", errors="replace")
            pod = struct.unpack_from(POD_FMT, mm, off + SLOT_OFF_POD)
            out[code] = pod
        return out


# ---------------------------------------------------------------------------
# TDengine 查询
# ---------------------------------------------------------------------------
def get_conn():
    return taosws.connect(TDENGINE_URL)


def _parse_ts(ts):
    if not isinstance(ts, str):
        return ts
    s = ts.split(" +")[0].split("+")[0].strip()
    for fmt in ("%Y-%m-%d %H:%M:%S", "%Y-%m-%d %H:%M:%S.%f", "%Y-%m-%d"):
        try:
            return datetime.strptime(s, fmt)
        except ValueError:
            continue
    return ts


def batch_query_1d(conn, days):
    """全市场 1d 批量查询 → {(market, code): [bars]}。一次查询替代 N 次逐股查询。

    按 (market, code) 聚合: tdx.kline 同 code 不同市场是独立子表 (如 k_sh000001
    上证指数 / k_sz000001 平安银行), 按 code 聚合会混合两者数据。
    """
    try:
        r = conn.query(
            f"SELECT market, code, ts, open, high, low, close, volume, amount "
            f"FROM tdx.kline WHERE cycle='1d' AND ts > NOW() - {days + 5}d"
        )
    except Exception:
        return {}
    out = defaultdict(list)
    for row in r:
        m, code = row[0], row[1]
        ts = _parse_ts(row[2])
        out[(m, code)].append({
            "ts": ts, "open": row[3], "high": row[4], "low": row[5],
            "close": row[6], "volume": float(row[7] or 0), "amount": float(row[8] or 0),
        })
    for mc in out:
        out[mc].sort(key=lambda b: b["ts"])
    return out


def _apply_qfq_bars(bars, events):
    """dict 列表版前复权 (借 common.apply_qfq)。仅改 OHLC, vol/amount 不变。

    scalper 的 bars 是 [{ts, open, high, low, close, volume, amount}, ...],
    转 DataFrame 复权后写回, 消除除权跳变对 calc_rps/has_limit_up 等跨日指标的影响。
    """
    if not events or not bars:
        return bars
    df = pd.DataFrame({
        "ts": pd.to_datetime([b["ts"] for b in bars]),
        "O": [b["open"] for b in bars],
        "H": [b["high"] for b in bars],
        "L": [b["low"] for b in bars],
        "C": [b["close"] for b in bars],
        "V": [b["volume"] for b in bars],
        "amount": [b["amount"] for b in bars],
    })
    df = apply_qfq(df, events)
    for i, b in enumerate(bars):
        b["open"] = df["O"].iloc[i]
        b["high"] = df["H"].iloc[i]
        b["low"] = df["L"].iloc[i]
        b["close"] = df["C"].iloc[i]
    return bars


def batch_query_intraday(conn, cycle="1m", days=None):
    """全市场分钟线批量查询 → {(market, code): [bars]}。days=None 查当日, 否则查最近 days 天。"""
    if days is None:
        where = f"cycle='{cycle}' AND ts >= TODAY()"
    else:
        where = f"cycle='{cycle}' AND ts > NOW() - {days + 2}d"
    try:
        r = conn.query(
            f"SELECT market, code, ts, open, high, low, close, volume, amount "
            f"FROM tdx.kline WHERE {where}"
        )
    except Exception:
        return {}
    out = defaultdict(list)
    for row in r:
        m, code = row[0], row[1]
        ts = _parse_ts(row[2])
        out[(m, code)].append({
            "ts": ts, "open": row[3], "high": row[4], "low": row[5],
            "close": row[6], "volume": float(row[7] or 0), "amount": float(row[8] or 0),
        })
    for mc in out:
        out[mc].sort(key=lambda b: b["ts"])
    return out


def batch_query_intraday_ndays(conn, cycle="1m", days=20, workers=8):
    """N 天分钟线批量查询 (只取 13:31 后 close, 供 calc_tail_momentum_factor)。

    数据量大, 优化: ① SQL 端过滤 13:31 前数据 ② 只取 close 列 ③ 按前缀分片并发。
    """
    # 13:31 = (13*3600 + 31*60) * 1000 ms = 48660000
    TAIL_MS = 48660000
    # market 限定前缀分片: 排除 sh000xxx / sz399xxx 指数混入查询 (对齐 all_mainboard_codes 的
    # (market, code) 联合过滤; 沪指 000xxx 与深股 00xxx 同前缀, 须按 market 区分)。
    chunks = [
        "(market='sh' AND code LIKE '60%')",
        "(market='sh' AND code LIKE '68%')",
        "(market='sz' AND code LIKE '00%')",
        "(market='sz' AND code LIKE '30%')",
        "(market='bj' AND code LIKE '43%')",
        "(market='bj' AND code LIKE '83%')",
        "(market='bj' AND code LIKE '87%')",
        "(market='bj' AND code LIKE '920%')",
    ]
    out = {}

    def _query_chunk(chunk_where):
        try:
            r = conn.query(
                f"SELECT market, code, ts, close FROM tdx.kline "
                f"WHERE cycle='{cycle}' AND ts > NOW() - {days + 2}d "
                f"AND (TIMETRUNCATE(ts, 1d) + {TAIL_MS}) <= ts "
                f"AND ({chunk_where})"
            )
            local = defaultdict(list)
            for row in r:
                m, code = row[0], row[1]
                ts = _parse_ts(row[2])
                local[(m, code)].append({
                    "ts": ts, "open": None, "high": None, "low": None,
                    "close": row[3], "volume": 0.0, "amount": 0.0,
                })
            for mc in local:
                local[mc].sort(key=lambda b: b["ts"])
            return local
        except Exception:
            return {}

    with ThreadPoolExecutor(max_workers=workers) as pool:
        futs = [pool.submit(_query_chunk, cw) for cw in chunks]
        for f in as_completed(futs):
            try:
                out.update(f.result())
            except Exception:
                pass
    return out


def batch_query_finance(conn):
    """全市场财务批量查询 → {code: {liutongguben, zongguben}}。取每只股最新一行。

    finance 表仅 code tag (无 market), 但按 code 安全: 指数/无财务标的不入此表,
    故 sh000001(上证指数) 与 sz000001(平安银行) 不会在 finance 冲突。下游 fin_all.get(code)。
    """
    try:
        r = conn.query(
            "SELECT code, LAST(liutongguben), LAST(zongguben) FROM tdx.finance GROUP BY code"
        )
    except Exception:
        return {}
    out = {}
    for row in r:
        code = row[0]
        out[code] = {"liutongguben": float(row[1] or 0), "zongguben": float(row[2] or 0)}
    return out


def fetch_recent_1m(conn, market, code, days=7):
    """取最近 days 天 1m bar → [(ts, close)]。verify 专用。"""
    tbl = f"tdx.k_{market}{code}_1m"
    try:
        r = conn.query(f"SELECT ts, close FROM {tbl} WHERE ts > NOW() - {days}d ORDER BY ts")
    except Exception:
        return []
    out = []
    for row in r:
        ts = _parse_ts(row[0])
        if not hasattr(ts, "date"):
            continue
        out.append((ts, float(row[1])))
    return out


def window_avg(bars, target_date, hm_lo, hm_hi):
    """target_date 上 [hm_lo, hm_hi) 分钟 close 算术平均 → (avg, n)。"""
    picks = [c for ts, c in bars
             if ts.date() == target_date and hm_lo <= ts.hour * 100 + ts.minute < hm_hi]
    if not picks:
        return None, 0
    return sum(picks) / len(picks), len(picks)


# ---------------------------------------------------------------------------
# 标的池
# ---------------------------------------------------------------------------
def read_wp_blk(path=WP_PATH):
    """读 WP.blk → [(market, code)]。每行 '前缀(1=sh/0=sz)+6位代码', 跳过 '1999999' 头标记。"""
    out = []
    try:
        with open(path) as f:
            for line in f:
                line = line.strip()
                if line == "1999999" or len(line) < 7:
                    continue
                if line[0] in "01" and line[1:7].isdigit():
                    out.append(("sh" if line[0] == "1" else "sz", line[1:7]))
    except FileNotFoundError:
        pass
    return out


# ---------------------------------------------------------------------------
# 基础指标计算
# ---------------------------------------------------------------------------
def has_limit_up(bars, threshold=LIMIT_UP_PCT, lookback=None):
    if len(bars) < 2:
        return False
    # 语义: 近 lookback 天内是否有某日涨停——只要求涨停当日落在窗口内,
    # 参照日 (前一日) 可早于窗口 (否则窗口边界的涨停会被漏掉)。
    cutoff = datetime.now() - timedelta(days=lookback) if lookback else None
    for i in range(1, len(bars)):
        if cutoff is not None and bars[i]["ts"] < cutoff:
            continue
        pre = bars[i - 1]["close"]
        if pre <= 0:
            continue
        cur = bars[i]["close"]
        if (cur - pre) / pre * 100 >= threshold:
            return True
    return False


def elapsed_minutes(epoch):
    t = time.localtime(epoch)
    h, m = t.tm_hour, t.tm_min
    mins = (h - 9) * 60 + m - 30
    if h >= 12:
        mins -= 90
    return max(1, min(240, mins))


def vwap_above(bars, ratio):
    if not bars:
        return 0.0, 0
    cum_vol = 0.0
    cum_amt = 0.0
    above = 0
    for b in bars:
        cum_vol += b["volume"]
        cum_amt += b["amount"]
        if cum_vol <= 0:
            continue
        vwap = cum_amt / cum_vol
        if b["close"] >= vwap:
            above += 1
    return above / len(bars), len(bars)


# ---------------------------------------------------------------------------
# 增强信号计算
# ---------------------------------------------------------------------------
def calc_tail_momentum_factor(bars_intraday_ndays, lookback, start_hhmm):
    if not bars_intraday_ndays:
        return None
    daily = {}
    for b in bars_intraday_ndays:
        ts = b["ts"]
        if not hasattr(ts, "hour"):
            continue
        if ts.hour * 100 + ts.minute < start_hhmm:
            continue
        dk = ts.strftime("%Y%m%d") if hasattr(ts, "strftime") else str(ts)[:10]
        daily.setdefault(dk, []).append(b["close"])

    cos_list, ar_list = [], []
    for dk in sorted(daily.keys())[-lookback:]:
        closes = daily[dk]
        if len(closes) < 10:
            continue
        rets = [closes[i] / closes[i - 1] - 1
                for i in range(1, len(closes)) if closes[i - 1] > 0]
        if len(rets) < 5:
            continue
        mean_r = sum(rets) / len(rets)
        er = [r - mean_r for r in rets]
        xs, ys = [], []
        for i in range(len(er) - 1):
            if er[i] > 0:
                xs.append(er[i])
                ys.append(er[i + 1])
        if len(xs) < 3:
            continue
        dot = sum(x * y for x, y in zip(xs, ys))
        nx = sum(x * x for x in xs) ** 0.5
        ny = sum(y * y for y in ys) ** 0.5
        if nx > 0 and ny > 0:
            cos_list.append(dot / (nx * ny))
        n = len(xs)
        xm, ym = sum(xs) / n, sum(ys) / n
        num = sum((x - xm) * (y - ym) for x, y in zip(xs, ys))
        den = sum((x - xm) ** 2 for x in xs)
        if den > 0:
            ar_list.append(num / den)

    if not cos_list and not ar_list:
        return None
    return {
        "cos": round(sum(cos_list) / len(cos_list), 4) if cos_list else None,
        "ar": round(sum(ar_list) / len(ar_list), 4) if ar_list else None,
        "days": max(len(cos_list), len(ar_list)),
    }


def calc_tail_vwema(bars_intraday, start_hhmm):
    if not bars_intraday:
        return None, None
    tail = [b for b in bars_intraday
            if hasattr(b["ts"], "hour") and b["ts"].hour * 100 + b["ts"].minute >= start_hhmm]
    if len(tail) < 6:
        return None, None

    def vwma(seg):
        pv = sum(b["close"] * b["volume"] for b in seg)
        v = sum(b["volume"] for b in seg)
        return pv / v if v > 0 else None

    full = vwma(tail)
    mid = len(tail) // 2
    fh, sh = tail[:mid], tail[mid:]
    fh_vwma, sh_vwma = vwma(fh), vwma(sh)
    fh_vol = sum(b["volume"] for b in fh)
    sh_vol = sum(b["volume"] for b in sh)
    diverging = bool(
        fh_vwma and sh_vwma and sh_vwma > fh_vwma
        and sh_vol >= fh_vol
    )
    return (round(full, 2) if full else None), diverging


def calc_cpr(high, low, close):
    if high <= 0 or low <= 0 or close <= 0:
        return None
    pivot = (high + low + close) / 3
    return {"upper": round(2 * pivot - low, 2),
            "pivot": round(pivot, 2),
            "lower": round(2 * pivot - high, 2)}


def calc_vol_high(bars_1d, lookback, mult=1.0):
    if not bars_1d or len(bars_1d) < 2:
        return {"is_high": False, "today": 0, "max_past": 0, "avg_mult": 0}
    today = bars_1d[-1]["amount"]
    past = bars_1d[-1 - lookback:-1] or bars_1d[:-1]
    max_past = max(b["amount"] for b in past)
    avg_past = sum(b["amount"] for b in past) / len(past) if past else 0
    avg_mult = today / avg_past if avg_past > 0 else 0
    is_high = today > max_past or (avg_past > 0 and today >= avg_past * mult)
    return {"is_high": is_high, "today": round(today, 0),
            "max_past": round(max_past, 0), "avg_mult": round(avg_mult, 2)}


def build_market_rps_baseline(bars_1d_all, periods):
    """全市场 RPS 基线 → {period: 排序后的 ret% 列表}。

    直接用已前复权的 bars_1d_all ({(market,code): bars}) 计算, 避免重复全表查询
    且保证复权一致 (原实现查未复权 close, 除权跳变污染 ret 分位)。
    样本为全市场个股 (已排除指数), 比原 '60%|00%' 前缀更完整、且无 market 混合。
    """
    baseline = {}
    for p in periods:
        rets = []
        for bars in bars_1d_all.values():
            if len(bars) > p:
                cur, past = bars[-1]["close"], bars[-1 - p]["close"]
                if past > 0:
                    rets.append((cur - past) / past * 100)
        rets.sort()
        baseline[p] = rets
    return baseline


def calc_rps(bars_1d, periods, baseline):
    if not bars_1d or not baseline:
        return {}
    cur = bars_1d[-1]["close"]
    if cur <= 0:
        return {}
    rps = {}
    for p in periods:
        bl = baseline.get(p)
        if not bl or len(bars_1d) <= p:
            continue
        past = bars_1d[-1 - p]["close"]
        if past <= 0:
            continue
        ret = (cur - past) / past * 100
        rank = bisect.bisect_right(bl, ret)
        rps[p] = round(rank / len(bl) * 100, 1)
    return rps


def calc_ma_position(bars_1d, periods):
    if not bars_1d:
        return {}
    cur = bars_1d[-1]["close"]
    result = {}
    for p in periods:
        if len(bars_1d) < p + 1:
            continue
        cur_ma = sum(b["close"] for b in bars_1d[-p:]) / p
        if len(bars_1d) >= p + 1:
            prev_ma = sum(b["close"] for b in bars_1d[-p - 1:-1]) / p
            rising = cur_ma > prev_ma
        else:
            rising = False
        result[p] = {
            "above": cur >= cur_ma,
            "rising": rising,
            "ma_value": round(cur_ma, 2),
        }
    return result


def calc_ma_alignment(bars_1d, periods):
    if not bars_1d:
        return False, {}
    mas = {}
    for p in periods:
        if len(bars_1d) >= p:
            mas[p] = sum(b["close"] for b in bars_1d[-p:]) / p
    if len(mas) != len(periods) or len(mas) < 2:
        return False, {p: round(v, 2) for p, v in mas.items()}
    bullish = all(mas[periods[i]] > mas[periods[i + 1]]
                  for i in range(len(periods) - 1))
    return bullish, {p: round(v, 2) for p, v in mas.items()}


def calc_breakout_quality(bars_today, bars_1d, close_ratio):
    if not bars_today or not bars_1d:
        return False, 0.0, False
    high = max(b["high"] for b in bars_today)
    close = bars_today[-1]["close"]
    if high <= 0:
        return False, 0.0, False
    ch_ratio = close / high
    today_vol = sum(b["volume"] for b in bars_today)
    past = bars_1d[-6:-1] if len(bars_1d) >= 6 else bars_1d[:-1]
    if past:
        avg_vol = sum(b["volume"] for b in past) / len(past)
        vol_expand = today_vol > avg_vol > 0
    else:
        vol_expand = False
    return ch_ratio >= close_ratio, round(ch_ratio, 3), vol_expand


def calc_turnover_stability(bars_1d, fin, turnover_2d_max):
    if not bars_1d or not fin or fin["liutongguben"] <= 0:
        return True, 0.0
    turnovers = []
    for b in bars_1d[-2:]:
        t = b["volume"] / fin["liutongguben"] * 100
        turnovers.append(t)
    if not turnovers:
        return True, 0.0
    avg_t = sum(turnovers) / len(turnovers)
    return avg_t <= turnover_2d_max, round(avg_t, 2)


# ---------------------------------------------------------------------------
# 法则判定
# ---------------------------------------------------------------------------
def diagnose(market, code, quote_pod, bars_1d, fin, bars_intraday,
             bars_intraday_ndays, cfg, names, rps_baseline=None):
    """对单股执行六步法则 + 增强信号，返回结果 dict (None = 被淘汰)。"""
    price = quote_pod[POD_PRICE]
    pre_close = quote_pod[POD_PRE_CLOSE]
    volume = quote_pod[POD_VOLUME]
    if pre_close <= 0 or price <= 0:
        return None

    result = {
        "code": f"{market}{code}",
        "name": names.get((market, code), ""),
        "price": price,
        "pre_close": pre_close,
        "volume": volume,
        "quote_time": time.strftime("%H:%M:%S", time.localtime(quote_pod[POD_TS])),
    }

    # 1. 涨幅
    gain = (price - pre_close) / pre_close * 100
    result["gain_pct"] = round(gain, 2)
    if not (cfg["gain_low"] <= gain <= cfg["gain_high"]):
        return None

    # 2. 股性 (涨停基因)
    result["has_limit_up"] = (has_limit_up(bars_1d, lookback=cfg["lookback_days"])
                              if bars_1d else False)
    if not result["has_limit_up"]:
        return None

    # 4. 市值
    if not fin or fin["liutongguben"] <= 0:
        return None
    cap = price * fin["liutongguben"] / 1e8
    result["cap_yi"] = round(cap, 2)
    if cap > cfg["cap_max"]:
        return None

    # 5. 换手率
    turnover = volume / fin["liutongguben"] * 100
    result["turnover_pct"] = round(turnover, 2)
    if not (cfg["turnover_low"] <= turnover <= cfg["turnover_high"]):
        return None

    # 3. 量比
    elapsed = elapsed_minutes(quote_pod[POD_TS])
    if bars_1d and len(bars_1d) >= 2:
        past5 = bars_1d[-6:-1] if len(bars_1d) >= 6 else bars_1d[:-1]
        avg_vol = sum(b["volume"] for b in past5) / len(past5)
        if avg_vol > 0:
            vol_ratio = (volume / elapsed) / (avg_vol / 240)
            result["vol_ratio"] = round(vol_ratio, 2)
            result["elapsed_min"] = elapsed
            if vol_ratio <= cfg["vol_ratio"]:
                return None
        else:
            result["vol_ratio"] = None
    else:
        result["vol_ratio"] = None

    # 6. 分时 VWAP
    if bars_intraday:
        ratio, n = vwap_above(bars_intraday, cfg["vwap_above_ratio"])
        result["vwap_above_ratio"] = round(ratio, 3)
        result["vwap_bars"] = n
        if ratio < cfg["vwap_above_ratio"]:
            return None
    else:
        result["vwap_above_ratio"] = None
        result["vwap_bars"] = 0

    # ===== 增强信号 =====
    enhancements = {}

    if bars_intraday_ndays:
        tmf = calc_tail_momentum_factor(
            bars_intraday_ndays, cfg["tail_mom_lookback"], cfg["tail_mom_start"])
        if tmf:
            enhancements["tail_mom"] = tmf

    vwema, diverging = calc_tail_vwema(bars_intraday, cfg["tail_mom_start"])
    if vwema is not None:
        enhancements["tail_vwema"] = vwema
        enhancements["vwema_diverging"] = diverging

    cpr = calc_cpr(quote_pod[POD_HIGH], quote_pod[POD_LOW], price)
    if cpr:
        enhancements["cpr"] = cpr

    enhancements["vol_high"] = calc_vol_high(
        bars_1d, cfg["vol_high_lookback"], cfg["vol_high_mult"])

    rps = calc_rps(bars_1d, cfg["rps_periods"], rps_baseline or {})
    if rps:
        enhancements["rps"] = rps

    ma_pos = calc_ma_position(bars_1d, cfg["ma_periods"])
    if ma_pos:
        enhancements["ma_position"] = ma_pos
    ma_bull, _ = calc_ma_alignment(bars_1d, cfg["ma_align_periods"])
    enhancements["ma_bullish_align"] = ma_bull

    is_quality, ch_ratio, vol_expand = calc_breakout_quality(
        bars_intraday, bars_1d, cfg["breakout_close_ratio"])
    enhancements["breakout_quality"] = is_quality
    enhancements["close_high_ratio"] = ch_ratio
    enhancements["vol_expand"] = vol_expand

    is_stable, avg_t2d = calc_turnover_stability(bars_1d, fin, cfg["turnover_2d_max"])
    enhancements["turnover_stable"] = is_stable
    enhancements["turnover_2d_avg"] = avg_t2d

    result["enhancements"] = enhancements

    # 综合信号得分
    checks = [
        ("tail_mom", lambda: enhancements.get("tail_mom", {}).get("cos") is not None
                              and enhancements["tail_mom"]["cos"] > 0),
        ("vwema_diverging", lambda: enhancements.get("vwema_diverging") is True),
        ("vol_high", lambda: enhancements.get("vol_high", {}).get("is_high") is True),
        ("breakout_quality", lambda: enhancements.get("breakout_quality") is True),
        ("turnover_stable", lambda: enhancements.get("turnover_stable") is True),
        ("ma_bullish", lambda: enhancements.get("ma_bullish_align") is True),
    ]
    hit, total = 0, 0
    for _, pred in checks:
        total += 1
        try:
            if pred():
                hit += 1
        except Exception:
            pass
    for p, val in rps.items():
        total += 1
        if val > 90:
            hit += 1
    for p, info in ma_pos.items():
        total += 1
        if info["above"] and info["rising"]:
            hit += 1
    result["signal_score"] = round(hit / max(1, total), 2) if total > 0 else 0
    result["signal_hit"] = hit
    result["signal_total"] = total
    result["pass"] = True
    return result


# ---------------------------------------------------------------------------
# 筛选主流程 (多线程)
# ---------------------------------------------------------------------------
def _screen_one_compute(market, code, pod, bars_1d, fin, intra, intra_ndays,
                        cfg, names, rps_baseline):
    """单股纯计算 (无 I/O, 供线程池调用)。"""
    return diagnose(market, code, pod, bars_1d, fin, intra, intra_ndays,
                    cfg, names, rps_baseline)


def prefetch_daily(conn, cfg, workers):
    """每交易日预取一次的日频/财务/N天分钟线 (盘中不变, 循环外缓存)。

    RPS 基线/1d/财务纯历史当日不变; N天1m 供尾盘动量因子 (看历史多日模式, 当日
    贡献小), 盘中缓存可接受。→ (rps_baseline, bars_1d_all, fin_all, intra_ndays_all)

    1d 日线拉取后批量应用前复权 (消除除权跳变), RPS 基线复用复权后的 bars 算。
    """
    t0 = time.time()
    # 1d 查询窗口须覆盖最长指标周期 (RPS 250 / MA 120); lookback_days 仅控制涨停基因回看。
    window = max(cfg["lookback_days"], cfg["rps_baseline_days"])
    bars_1d_all = batch_query_1d(conn, window)
    adj_by_mc = batch_fetch_adjust(conn, list(bars_1d_all.keys()))
    for mc in bars_1d_all:
        bars_1d_all[mc] = _apply_qfq_bars(bars_1d_all[mc], adj_by_mc.get(mc))
    rps_baseline = build_market_rps_baseline(bars_1d_all, cfg["rps_periods"])
    fin_all = batch_query_finance(conn)
    intra_ndays_all = batch_query_intraday_ndays(
        conn, "1m", days=cfg["tail_mom_lookback"], workers=workers)
    sys.stderr.write(
        f"[scalper] 日频预取 (1d复权/RPS基线/财务/N天1m) 耗时 {time.time() - t0:.1f}s\n")
    return rps_baseline, bars_1d_all, fin_all, intra_ndays_all


def screen(conn, universe, shm_reader, cfg, names, daily,
           require_intraday=True, workers=8):
    """单轮筛选: daily=预取的日频数据(盘中不变), 每轮只刷当日 1m + 取 shm pod。"""
    rps_baseline, bars_1d_all, fin_all, intra_ndays_all = daily
    t0 = time.time()

    # ---- 当日分钟线 (盘中实时变化, 每轮刷新) ----
    intra_all = {}
    if require_intraday:
        intra_all = batch_query_intraday(conn, "1m", days=None)
        sys.stderr.write(f"[scalper] 批量查询当日 1m 耗时 {time.time() - t0:.1f}s\n")

    # ---- 纯计算并行 (日频/N天1m 取自 daily 缓存, 不每轮查) ----
    t1 = time.time()
    results, errs, futs = [], 0, []
    with ThreadPoolExecutor(max_workers=workers) as pool:
        for m, c in universe:
            pod = shm_reader.get(c)
            if pod is None:
                continue
            bars_1d = bars_1d_all.get((m, c))
            if not bars_1d:
                continue
            fin = fin_all.get(c)  # finance 仅 code tag, 按 code 取 (指数无财务, 不混合)
            intra = intra_all.get((m, c)) if require_intraday else None
            intra_ndays = intra_ndays_all.get((m, c)) if require_intraday else None
            futs.append(pool.submit(
                _screen_one_compute, m, c, pod, bars_1d, fin,
                intra, intra_ndays, cfg, names, rps_baseline))
        for f in as_completed(futs):
            try:
                r = f.result()
                if r:
                    results.append(r)
            except Exception:
                errs += 1
    if errs:
        sys.stderr.write(f"[scalper] ⚠ {errs}/{len(futs)} 只计算异常 (已跳过)\n")
    sys.stderr.write(f"[scalper] {workers} 线程计算 {len(futs)} 只耗时 {time.time() - t1:.1f}s\n")
    results.sort(key=lambda x: (-x.get("signal_score", 0), -x["gain_pct"]))
    return results


# ---------------------------------------------------------------------------
# 表格渲染
# ---------------------------------------------------------------------------
def render_table(results, round_no, elapsed_sec):
    """ANSI 清屏 + 表格输出 (基础指标 + 增强信号合并到一张表)。"""
    ts = time.strftime("%H:%M:%S")
    lines = []
    lines.append("\033[2J\033[H")  # 清屏
    lines.append(f"=== 隔夜套利战法选股 (时空置换) ===   第 {round_no} 轮  "
                 f"{ts}  耗时 {elapsed_sec:.1f}s  命中 {len(results)} 只")
    lines.append("")

    if not results:
        lines.append("  暂无标的通过六步法则。")
        lines.append("")
        lines.append("⚔ 铁的纪律: 次日早盘 (10:30 前) 无论盈亏必须清仓，持股不超 4 小时。")
        return "\n".join(lines) + "\n"

    # 表头: 基础指标 + 增强信号 (宽度均为显示宽度, 与数据行对齐)
    hdr = (pad("代码", 8) + pad("名称", 8)
           + pad("现价", 8, ">") + pad("涨幅%", 8, ">") + pad("量比", 7, ">")
           + pad("换手%", 8, ">") + pad("市值亿", 10, ">") + pad("VWAP上", 8, ">")
           + pad("尾盘动量", 10, ">") + pad("VWEMA", 9, ">") + pad("量能", 8, ">")
           + pad("RPS", 7, ">") + pad("MA", 7, ">") + pad("突破", 10, ">")
           + pad("得分", 6, ">"))
    lines.append(hdr)
    lines.append("-" * disp_w(hdr))

    for r in results:
        enh = r.get("enhancements", {})

        # 基础列
        vwap_s = f"{r['vwap_above_ratio']*100:.0f}%" if r.get('vwap_above_ratio') is not None else "N/A"
        vr_s = f"{r['vol_ratio']:.2f}" if r.get('vol_ratio') is not None else "N/A"

        # 尾盘动量 (cos 符号 + 颜色)
        tm = enh.get("tail_mom")
        if tm and tm.get("cos") is not None:
            cos_v = tm["cos"]
            tm_s = f"{cos_v:+.2f}"
            if cos_v > 0:
                tm_s = f"\033[1;31m{tm_s}\033[0m"  # 红
            else:
                tm_s = f"\033[1;32m{tm_s}\033[0m"  # 绿
        else:
            tm_s = "-"

        # VWEMA 发散
        if enh.get("vwema_diverging") is True:
            vw_s = "\033[1;31m↑发散\033[0m"
        elif enh.get("vwema_diverging") is False:
            vw_s = "─"
        else:
            vw_s = "-"

        # 量能 (新高 / 均额倍数)
        vh = enh.get("vol_high")
        if vh:
            tag = "\033[1;31m新高\033[0m" if vh["is_high"] else "─"
            vh_s = f"{tag}{vh['avg_mult']:.1f}×"
        else:
            vh_s = "-"

        # RPS (最大百分位)
        rps = enh.get("rps") or {}
        if rps:
            rps_max = max(rps.values())
            rps_s = f"{'★' if rps_max > 90 else ' '}{rps_max:.0f}"
        else:
            rps_s = "-"

        # MA 排列
        if enh.get("ma_bullish_align") is True:
            ma_s = "\033[1;31m多排\033[0m"
        elif enh.get("ma_bullish_align") is False:
            ma_s = "─"
        else:
            ma_s = "-"

        # 突破质量
        if enh.get("breakout_quality") is True:
            bq_s = "\033[1;31m真\033[0m"
        elif enh.get("breakout_quality") is False:
            bq_s = "假"
        else:
            bq_s = "-"

        # 得分
        score = r.get('signal_score', 0)
        score_s = f"{score:.0%}" if score is not None else "-"
        if score >= 0.6:
            score_s = f"\033[1;33m{score_s}\033[0m"

        lines.append(pad(r["code"], 8) + pad(r.get("name") or "-", 8)
                     + pad(f"{r['price']:.2f}", 8, ">") + pad(f"{r['gain_pct']:.2f}", 8, ">")
                     + pad(vr_s, 7, ">") + pad(f"{r['turnover_pct']:.2f}", 8, ">")
                     + pad(f"{r['cap_yi']:.2f}", 10, ">") + pad(vwap_s, 8, ">")
                     + pad(tm_s, 10, ">") + pad(vw_s, 9, ">") + pad(vh_s, 8, ">")
                     + pad(rps_s, 7, ">") + pad(ma_s, 7, ">") + pad(bq_s, 10, ">")
                     + pad(score_s, 6, ">"))

    lines.append("")
    lines.append("⚔ 铁的纪律: 次日早盘 (10:30 前) 无论盈亏必须清仓，持股不超 4 小时。")
    lines.append("📊 得分 = 增强信号命中数 / 信号总数，≥60% 为优选标 (★=RPS>90)。")
    return "\n".join(lines) + "\n"


# ---------------------------------------------------------------------------
# 时间工具
# ---------------------------------------------------------------------------
def parse_hhmm(s):
    """解析 HH:MM → (hour, minute)"""
    h, m = s.split(":")
    return int(h), int(m)


def today_at(h, m):
    """返回今天 HH:MM 的 epoch"""
    now = datetime.now()
    return now.replace(hour=h, minute=m, second=0, microsecond=0)


def wait_until(target_dt):
    """阻塞等待到目标时间，返回是否等到 (False=已过期)。"""
    now = datetime.now()
    if now >= target_dt:
        return False
    delta = (target_dt - now).total_seconds()
    sys.stderr.write(f"[scalper] 等待至 {target_dt.strftime('%H:%M')} (还有 {delta:.0f}s)...\n")
    # 先 sleep 大段，最后 5s 轮询
    if delta > 5:
        time.sleep(delta - 5)
    while datetime.now() < target_dt:
        time.sleep(0.5)
    return True


# ---------------------------------------------------------------------------
# 验证流程 (verify 子命令)
# ---------------------------------------------------------------------------
def run_verify(args):
    conn = get_conn()
    ensure_scalper_tables(conn)
    names = {}
    try:
        for m, c, n in conn.query("SELECT market, code, name FROM tdx.stock_name"):
            names[(m, c)] = n
    except Exception:
        pass

    stocks = read_wp_blk(args.wp)
    if not stocks:
        sys.stderr.write(f"[verify] {args.wp} 无标的 (需先在前一交易日 14:30 运行选股写入 WP.blk)\n")
        return 1
    sys.stderr.write(f"[verify] {args.wp} 读到 {len(stocks)} 只标的\n")

    if args.date:
        today = datetime.strptime(args.date, "%Y-%m-%d").date()
    else:
        today = datetime.now().date()

    # 卖出窗口 [09:30,10:00) 当日: 实时验证时等待至 10:00, 避免数据不全
    if not args.no_wait and not args.date:
        wait_until(today_at(10, 0))

    capital = VERIFY_CAPITAL
    per = capital / len(stocks)
    rows, total_pnl, valid = [], 0.0, 0
    for market, code in stocks:
        bars = fetch_recent_1m(conn, market, code, args.days)
        dates_before = sorted({ts.date() for ts, _ in bars if ts.date() < today})
        buy_date = dates_before[-1] if dates_before else None
        buy, bn = window_avg(bars, buy_date, 1430, 1500) if buy_date else (None, 0)
        sell, sn = window_avg(bars, today, 930, 1000)

        row = {
            "verify_date": today.strftime("%Y-%m-%d"),
            "code": f"{market}{code}", "name": names.get((market, code), ""),
            "buy_date": buy_date.strftime("%Y-%m-%d") if buy_date else "",
            "buy_price": round(buy, 4) if buy else "",
            "buy_bars": bn,
            "sell_date": today.strftime("%Y-%m-%d") if sell else "",
            "sell_price": round(sell, 4) if sell else "",
            "sell_bars": sn,
            "shares": "", "cost": "", "income": "", "pnl": "", "pnl_pct": "",
        }
        if buy and sell and buy > 0:
            shares = per / buy  # ponytail: 连续股数不取整, 实盘 100 股最小单位 + 手续费/印花税未计
            income = shares * sell
            pnl = income - per
            row.update(shares=round(shares), cost=round(per, 2),
                       income=round(income, 2), pnl=round(pnl, 2),
                       pnl_pct=round((sell - buy) / buy * 100, 2))
            total_pnl += pnl
            valid += 1
        rows.append(row)

    total_pct = total_pnl / capital * 100
    rows.append({
        "verify_date": today.strftime("%Y-%m-%d"),
        "code": "TOTAL", "name": f"{valid}/{len(stocks)}有效",
        "buy_date": "", "buy_price": "", "buy_bars": "",
        "sell_date": "", "sell_price": "", "sell_bars": "",
        "shares": "", "cost": round(capital, 2), "income": "",
        "pnl": round(total_pnl, 2), "pnl_pct": round(total_pct, 2),
    })

    append_verify_csv(args.out, rows, today.strftime("%Y-%m-%d"))
    upsert_verify(conn, rows, today.strftime("%Y-%m-%d"))
    print_verify(rows, capital)
    return 0


def append_verify_csv(out_path, rows, date_str):
    """追加验证行到 CSV, 同日去重 (重跑覆盖当日)。"""
    os.makedirs(os.path.dirname(out_path) or ".", exist_ok=True)
    kept = []
    if os.path.exists(out_path):
        with open(out_path, newline="") as f:
            for r in csv.DictReader(f):
                if r.get("verify_date") != date_str:
                    kept.append(r)
    with open(out_path, "w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=VERIFY_FIELDS)
        w.writeheader()
        for r in kept + rows:
            w.writerow({k: r.get(k, "") for k in VERIFY_FIELDS})
    n = sum(1 for r in rows if r["code"] != "TOTAL")
    sys.stderr.write(f"[verify] → {out_path} (当日 {n} 只 + 汇总)\n")


def print_verify(rows, capital):
    n = sum(1 for r in rows if r["code"] != "TOTAL")
    sep = "-" * 60
    lines = [f"=== 隔夜套利战法验证 ===  {time.strftime('%Y-%m-%d %H:%M:%S')}",
             f"资金 {capital:.0f} 元, 等额分配 {capital / max(1, n):.0f} 元/只",
             (pad("代码", 10) + pad("名称", 8) + pad("买价", 9, ">") + pad("卖价", 9, ">")
              + pad("盈亏", 10, ">") + pad("收益%", 8, ">")),
             sep]
    for r in rows:
        if r["code"] == "TOTAL":
            lines.append(sep)
            lines.append(pad("合计 " + r["name"], 18) + pad("", 9, ">") + pad("", 9, ">")
                         + pad(f"{r['pnl']:.2f}", 10, ">") + pad(f"{r['pnl_pct']:.2f}", 8, ">"))
        elif r["pnl"] == "":
            bp, sp = r["buy_price"] or "-", r["sell_price"] or "-"
            lines.append(pad(r["code"], 10) + pad(r["name"], 8) + pad(str(bp), 9, ">")
                         + pad(str(sp), 9, ">") + pad("缺数据", 10, ">") + pad("-", 8, ">"))
        else:
            lines.append(pad(r["code"], 10) + pad(r["name"], 8)
                         + pad(f"{r['buy_price']:.4f}", 9, ">") + pad(f"{r['sell_price']:.4f}", 9, ">")
                         + pad(f"{r['pnl']:.2f}", 10, ">") + pad(f"{r['pnl_pct']:.2f}", 8, ">"))
    sys.stdout.write("\n".join(lines) + "\n")
    sys.stdout.flush()


# ---------------------------------------------------------------------------
# TDengine 持久化 (选股/验证结果带日期戳入库, 方便查询统计)
# ---------------------------------------------------------------------------
def _esc(v):
    return str(v).replace("'", "''")


def _num(v):
    """数值 → SQL 字面量, None/空串 → NULL。"""
    if v is None or v == "":
        return "NULL"
    try:
        return repr(float(v))
    except (TypeError, ValueError):
        return "NULL"


def ensure_scalper_tables(conn):
    conn.execute("USE tdx")
    conn.execute(PICK_DDL)
    conn.execute(VERIFY_DDL)


def _day_window(date_str):
    """返回 [date_str, 次日) 的 SQL 时间窗 (DELETE 必须带时间窗)。"""
    nxt = (datetime.strptime(date_str, "%Y-%m-%d") + timedelta(days=1)).strftime("%Y-%m-%d")
    return f"ts >= '{date_str}' AND ts < '{nxt}'"


def upsert_pick(conn, results, pick_date):
    """选股结果 upsert scalper_pick (ts=当日0点)。幂等: 先删当日窗再插。"""
    if not results:
        return
    ts = f"{pick_date} 00:00:00"
    try:
        conn.execute(f"DELETE FROM scalper_pick WHERE {_day_window(pick_date)}")
    except Exception as e:
        sys.stderr.write(f"[pick-td] DELETE 失败: {e}\n")
    for r in results:
        full = r["code"]  # sh603580
        market, code = full[:2], full[2:]
        # taosws 查询返回 numpy 标量 (np.bool_/np.int64 非 Python int/bool 子类),
        # 比较运算 (如 MA above/rising) 产生 np.bool_ 会被 json.dumps 拒收 → default 还原原生类型
        enh = json.dumps(r.get("enhancements", {}), ensure_ascii=False,
                         default=lambda o: o.item() if hasattr(o, "item") else str(o))
        sql = (f"INSERT INTO sp_{market}{code} USING scalper_pick "
               f"TAGS('{_esc(code)}','{market}','{_esc(r.get('name', ''))}') VALUES("
               f"'{ts}',{_num(r.get('price'))},{_num(r.get('gain_pct'))},"
               f"{_num(r.get('vol_ratio'))},{_num(r.get('turnover_pct'))},"
               f"{_num(r.get('cap_yi'))},{_num(r.get('vwap_above_ratio'))},"
               f"{_num(r.get('signal_score'))},{int(r.get('signal_hit') or 0)},"
               f"{int(r.get('signal_total') or 0)},'{_esc(enh)}')")
        try:
            conn.execute(sql)
        except Exception as e:
            sys.stderr.write(f"[pick-td] {full} 写入失败: {e}\n")
    sys.stderr.write(f"[pick-td] → scalper_pick ({len(results)} 只)\n")


def upsert_verify(conn, rows, verify_date):
    """验证结果 upsert scalper_verify (ts=验证日0点, 含 TOTAL 汇总行)。"""
    ts = f"{verify_date} 00:00:00"
    try:
        conn.execute(f"DELETE FROM scalper_verify WHERE {_day_window(verify_date)}")
    except Exception as e:
        sys.stderr.write(f"[verify-td] DELETE 失败: {e}\n")
    n = 0
    for r in rows:
        full = r["code"]  # sh603580 或 TOTAL
        if full == "TOTAL":
            sub, market, code = "sv_total", "", "TOTAL"
        else:
            market, code = full[:2], full[2:]
            sub = f"sv_{market}{code}"
        sql = (f"INSERT INTO {sub} USING scalper_verify "
               f"TAGS('{_esc(code)}','{market}','{_esc(r.get('name', ''))}') VALUES("
               f"'{ts}','{_esc(r.get('buy_date', ''))}',{_num(r.get('buy_price'))},"
               f"{int(r.get('buy_bars') or 0)},{_num(r.get('sell_price'))},"
               f"{int(r.get('sell_bars') or 0)},{_num(r.get('shares'))},"
               f"{_num(r.get('cost'))},{_num(r.get('income'))},{_num(r.get('pnl'))},"
               f"{_num(r.get('pnl_pct'))})")
        try:
            conn.execute(sql)
            if full != "TOTAL":
                n += 1
        except Exception as e:
            sys.stderr.write(f"[verify-td] {full} 写入失败: {e}\n")
    sys.stderr.write(f"[verify-td] → scalper_verify ({n} 只 + 汇总)\n")


# ---------------------------------------------------------------------------
# 主流程
# ---------------------------------------------------------------------------
def main():
    # verify 子命令: 验证前日尾盘选股的隔夜套利收益
    if len(sys.argv) > 1 and sys.argv[1] == "verify":
        ap = argparse.ArgumentParser(prog="scalper.py verify",
                                     description="隔夜套利验证: 买入=前日14:30-15:00均价, 卖出=当日9:30-10:00均价")
        ap.add_argument("--wp", default=WP_PATH, help="板块文件 (默认 WP.blk)")
        ap.add_argument("--date", help="验证日期 YYYY-MM-DD (默认今天, 用于回测)")
        ap.add_argument("--days", type=int, default=7, help="回看天数 (回测远日时调大)")
        ap.add_argument("--no-wait", action="store_true", help="不等 10:00, 立即计算")
        ap.add_argument("--out", default=VERIFY_CSV, help="输出 CSV 路径")
        return run_verify(ap.parse_args(sys.argv[2:]))

    ap = argparse.ArgumentParser(description="隔夜套利战法选股 (六步法则 + 增强信号) —— 尾盘循环版")
    ap.add_argument("--all", action="store_true", help="全量主板标的 (默认仅自选股)")
    ap.add_argument("--code", help="单股诊断 (如 sh600000)")
    ap.add_argument("--shm", default=DEFAULT_SHM, help="共享内存路径")
    ap.add_argument("--lookback", type=int, default=DEFAULTS["lookback_days"], help="涨停基因回看天数")
    ap.add_argument("--no-intraday", action="store_true", help="跳过分时 VWAP 判定")
    ap.add_argument("--gain-low", type=float, default=DEFAULTS["gain_low"])
    ap.add_argument("--gain-high", type=float, default=DEFAULTS["gain_high"])
    ap.add_argument("--cap-max", type=float, default=DEFAULTS["cap_max"])
    ap.add_argument("--vol-ratio", type=float, default=DEFAULTS["vol_ratio"])
    ap.add_argument("--tail-lookback", type=int, default=DEFAULTS["tail_mom_lookback"])
    ap.add_argument("--start", default="14:30", help="启动时间 HH:MM (默认 14:30)")
    ap.add_argument("--end", default="15:00", help="结束时间 HH:MM (默认 15:00)")
    ap.add_argument("--interval", type=int, default=30, help="刷新间隔秒 (默认 30)")
    ap.add_argument("--once", action="store_true", help="单次运行 (不循环)")
    ap.add_argument("--workers", type=int, default=8, help="并发线程数 (默认 8)")
    args = ap.parse_args()

    cfg = dict(DEFAULTS)
    cfg["lookback_days"] = args.lookback
    cfg["gain_low"] = args.gain_low
    cfg["gain_high"] = args.gain_high
    cfg["cap_max"] = args.cap_max
    cfg["vol_ratio"] = args.vol_ratio
    cfg["tail_mom_lookback"] = args.tail_lookback

    # 解析时段
    start_h, start_m = parse_hhmm(args.start)
    end_h, end_m = parse_hhmm(args.end)

    # 打开 shm
    shm = ShmReader(args.shm)
    if not shm.open():
        sys.stderr.write(f"❌ 共享内存未就绪: {args.shm} (需先运行 fetch-quotes 写 shm)\n")
        return 1

    conn = get_conn()
    ensure_scalper_tables(conn)

    # 股票名称对照
    names = {}
    try:
        for m, c, n in conn.query("SELECT market, code, name FROM tdx.stock_name"):
            names[(m, c)] = n
    except Exception:
        pass

    # 标的池
    if args.code:
        market, code = parse_code(args.code)
        universe = [(market, code)]
    elif args.all:
        # 战法规则与阈值 (涨幅/涨停基因 9.8%/换手) 均针对 10% 涨停的主板 (60/00);
        # 创业板/科创/北交涨跌幅不同, 统一 9.8% 判定失真 → --all 限定主板。
        universe = [(m, c) for m, c in all_mainboard_codes(conn)
                    if c.startswith(MAIN_BOARD_PREFIXES)]
    else:
        universe = [(parse_code(c)[0], parse_code(c)[1]) for c in zxg_codes()]

    # 预过滤 (仅按 shm 存活过滤, 表存在性由 batch 查询 .get() 兜底)
    if not args.code:
        shm_codes = set(shm.all_quotes().keys())
        universe = [(m, c) for m, c in universe if c in shm_codes]
        sys.stderr.write(f"[scalper] 标的池: {len(universe)} 只\n")

    require_intraday = not args.no_intraday
    round_no = 0
    start_epoch = time.time()
    daily = None
    cur_date = None

    try:
        while True:
            # 等待启动时间
            target_start = today_at(start_h, start_m)
            now = datetime.now()
            if now < target_start:
                if not wait_until(target_start):
                    continue

            # 检查是否已过结束时间
            now = datetime.now()
            end_dt = today_at(end_h, end_m)
            if now >= end_dt:
                elapsed_min = (time.time() - start_epoch) / 60
                sys.stderr.write(f"[scalper] 已到结束时间 {args.end}，"
                                 f"共运行 {elapsed_min:.1f} 分钟，退出。\n")
                break

            # 日频预取按交易日缓存 (盘中不变, 跨日才重取)
            today = now.date()
            if daily is None or today != cur_date:
                daily = prefetch_daily(conn, cfg, args.workers)
                cur_date = today

            # 执行一轮筛选
            round_no += 1
            t0 = time.time()
            sys.stderr.write(f"[scalper] 第 {round_no} 轮筛选开始...\n")
            try:
                results = screen(conn, universe, shm, cfg, names, daily,
                                 require_intraday=require_intraday,
                                 workers=args.workers)
                elapsed = time.time() - t0
                output = render_table(results, round_no, elapsed)
                sys.stdout.write(output)
                sys.stdout.flush()
                # 保存结果
                if results:
                    blk_dir = os.path.dirname(WP_PATH)
                    os.makedirs(blk_dir, exist_ok=True)
                    # WP.blk (通达信自选板块)
                    with open(WP_PATH, "w", newline="") as f:
                        f.write("1999999\r\n")
                        for r in results:
                            full = r["code"]
                            if full.startswith("bj"):
                                continue  # blk 格式仅 1=sh/0=sz 两位前缀, 无法表示北交所
                            prefix = "1" if full.startswith("sh") else "0"
                            f.write(f"{prefix}{full[2:]}\r\n")
                    sys.stderr.write(f"[blk] → WP.blk ({len(results)} 只)\n")
                    # 本地 CSV
                    csv_dir = "output/scalper"
                    os.makedirs(csv_dir, exist_ok=True)
                    csv_file = os.path.join(csv_dir, f"scalper-{time.strftime('%Y%m%d')}.csv")
                    fields = list(results[0].keys())
                    with open(csv_file, "w", newline="") as cf:
                        w = csv.DictWriter(cf, fieldnames=fields)
                        w.writeheader()
                        for r in results:
                            row = {}
                            for k, v in r.items():
                                row[k] = f"{v:.3f}" if isinstance(v, float) else v
                            w.writerow(row)
                    sys.stderr.write(f"[csv] → {csv_file}\n")
                    upsert_pick(conn, results, time.strftime("%Y-%m-%d"))
            except Exception as e:
                elapsed = time.time() - t0
                sys.stderr.write(f"[scalper] 第 {round_no} 轮异常: {e}\n")

            if args.once:
                break

            # 等待下一轮
            time.sleep(args.interval)
    except KeyboardInterrupt:
        sys.stderr.write("\n[scalper] 收到 Ctrl-C，退出。\n")
        return 130
    finally:
        shm.close()

    return 0


if __name__ == "__main__":
    sys.exit(main())