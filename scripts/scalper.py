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
import json
import mmap
import os
import struct
import sys
import time
from collections import defaultdict
from datetime import datetime, timedelta

import taosws

# ---------------------------------------------------------------------------
# 配置
# ---------------------------------------------------------------------------
TDENGINE_URL = "taosws://root:taosdata@localhost:6041"
DEFAULT_SHM = "/dev/shm/tdx_quotes.shm"
ZXG_PATH = "/home/li/.local/share/tdxcfv/drive_c/tc/T0002/blocknew/zxg.blk"

# shm 二进制布局 (与 include/tdx/shm/{segment,snapshot,payload}.hpp 一致)
POD_FMT = "< q 27d"  # datetime(i64) + 27×double = 224B
assert struct.calcsize(POD_FMT) == 224
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


def query_1d(conn, code, market, days):
    tbl = f"tdx.k_{market}{code}_1d"
    try:
        r = conn.query(
            f"SELECT ts, open, high, low, close, volume, amount "
            f"FROM {tbl} WHERE ts > NOW() - {days + 5}d ORDER BY ts"
        )
    except Exception:
        return None
    bars = []
    for row in r:
        bars.append({
            "ts": _parse_ts(row[0]), "open": row[1], "high": row[2], "low": row[3],
            "close": row[4], "volume": float(row[5] or 0), "amount": float(row[6] or 0),
        })
    return bars if bars else None


def query_intraday(conn, code, market, cycle="1m"):
    tbl = f"tdx.k_{market}{code}_{cycle}"
    try:
        r = conn.query(
            f"SELECT ts, open, high, low, close, volume, amount "
            f"FROM {tbl} WHERE ts >= TODAY() ORDER BY ts"
        )
    except Exception:
        return None
    bars = []
    for row in r:
        bars.append({
            "ts": _parse_ts(row[0]), "open": row[1], "high": row[2], "low": row[3],
            "close": row[4], "volume": float(row[5] or 0), "amount": float(row[6] or 0),
        })
    return bars if bars else None


def query_intraday_ndays(conn, code, market, days, cycle="1m"):
    tbl = f"tdx.k_{market}{code}_{cycle}"
    try:
        r = conn.query(
            f"SELECT ts, open, high, low, close, volume, amount "
            f"FROM {tbl} WHERE ts > NOW() - {days + 2}d ORDER BY ts"
        )
    except Exception:
        return None
    bars = []
    for row in r:
        bars.append({
            "ts": _parse_ts(row[0]), "open": row[1], "high": row[2], "low": row[3],
            "close": row[4], "volume": float(row[5] or 0), "amount": float(row[6] or 0),
        })
    return bars if bars else None


def query_finance(conn, code):
    tbl = f"tdx.fn_{code}"
    try:
        r = conn.query(f"SELECT liutongguben, zongguben FROM {tbl} ORDER BY ts DESC LIMIT 1")
    except Exception:
        return None
    for row in r:
        return {"liutongguben": float(row[0] or 0), "zongguben": float(row[1] or 0)}
    return None


def table_exists(conn, tbl):
    try:
        r = conn.query(
            f"SELECT table_name FROM information_schema.ins_tables "
            f"WHERE db_name='tdx' AND table_name='{tbl}'"
        )
        for _ in r:
            return True
    except Exception:
        return False
    return False


# ---------------------------------------------------------------------------
# 标的池
# ---------------------------------------------------------------------------
def zxg_codes(path=ZXG_PATH):
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
    try:
        r = conn.query(
            "SELECT code, market FROM tdx.stock_name "
            f"WHERE code LIKE '60%' OR code LIKE '00%'"
        )
        return [(m, c) for c, m in r]
    except Exception:
        return []


def parse_code(code):
    code = code.strip().lower()
    if code[:2] in ("sh", "sz", "bj"):
        return code[:2], code[2:]
    if code.startswith(("60", "68", "99")):
        return "sh", code
    if code.startswith(("00", "30", "39")):
        return "sz", code
    if code.startswith(("8", "4")):
        return "bj", code
    return "sh", code


# ---------------------------------------------------------------------------
# 基础指标计算
# ---------------------------------------------------------------------------
def has_limit_up(bars, threshold=LIMIT_UP_PCT):
    if len(bars) < 2:
        return False
    for i in range(1, len(bars)):
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


def build_market_rps_baseline(conn, periods, baseline_days):
    by_code = defaultdict(list)
    try:
        r = conn.query(
            f"SELECT code, close FROM tdx.kline "
            f"WHERE cycle='1d' AND (code LIKE '60%' OR code LIKE '00%') "
            f"AND ts > NOW() - {baseline_days}d"
        )
        for code, close in r:
            if close is not None:
                by_code[code].append(close)
    except Exception:
        return {}

    baseline = {}
    for p in periods:
        rets = []
        for closes in by_code.values():
            if len(closes) > p:
                cur, past = closes[-1], closes[-1 - p]
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
def diagnose(market, code, conn, quote_pod, bars_1d, fin, bars_intraday,
             bars_intraday_ndays, cfg, rps_baseline=None):
    """对单股执行六步法则 + 增强信号，返回结果 dict (None = 被淘汰)。"""
    price = quote_pod[1]
    pre_close = quote_pod[2]
    volume = quote_pod[6]
    if pre_close <= 0 or price <= 0:
        return None

    result = {
        "code": f"{market}{code}",
        "price": price,
        "pre_close": pre_close,
        "volume": volume,
        "quote_time": time.strftime("%H:%M:%S", time.localtime(quote_pod[0])),
    }

    # 1. 涨幅
    gain = (price - pre_close) / pre_close * 100
    result["gain_pct"] = round(gain, 2)
    if not (cfg["gain_low"] <= gain <= cfg["gain_high"]):
        return None

    # 2. 股性 (涨停基因)
    result["has_limit_up"] = has_limit_up(bars_1d) if bars_1d else False
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
    elapsed = elapsed_minutes(quote_pod[0])
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

    cpr = calc_cpr(quote_pod[4], quote_pod[5], price)
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
# 筛选主流程
# ---------------------------------------------------------------------------
def screen(conn, universe, shm_reader, cfg, require_intraday=True):
    rps_baseline = build_market_rps_baseline(
        conn, cfg["rps_periods"], cfg["rps_baseline_days"])
    results = []
    for i, (market, code) in enumerate(universe):
        pod = shm_reader.get(code)
        if pod is None:
            continue
        bars_1d = query_1d(conn, code, market, cfg["lookback_days"])
        if not bars_1d:
            continue
        fin = query_finance(conn, code)
        intra = query_intraday(conn, code, market, "1m") if require_intraday else None
        intra_ndays = None
        if require_intraday:
            intra_ndays = query_intraday_ndays(
                conn, code, market, cfg["tail_mom_lookback"], "1m")
        r = diagnose(market, code, conn, pod, bars_1d, fin, intra, intra_ndays,
                     cfg, rps_baseline)
        if r:
            results.append(r)
    results.sort(key=lambda x: (-x.get("signal_score", 0), -x["gain_pct"]))
    return results


# ---------------------------------------------------------------------------
# 表格渲染
# ---------------------------------------------------------------------------
def render_table(results, round_no, elapsed_sec):
    """ANSI 清屏 + 表格输出。"""
    ts = time.strftime("%H:%M:%S")
    lines = []
    lines.append("\033[2J\033[H")  # 清屏
    lines.append(f"=== 隔夜套利战法选股 (时空置换) ===   第 {round_no} 轮  "
                 f"{ts}  耗时 {elapsed_sec:.1f}s  命中 {len(results)} 只")
    lines.append("")

    if not results:
        lines.append("  暂无标的通过六步法则。")
        return "\n".join(lines) + "\n"

    # 汇总表
    hdr = (f"{'代码':<10}{'现价':>8}{'涨幅%':>8}{'量比':>7}{'换手%':>8}"
           f"{'市值亿':>10}{'VWAP上':>8}{'得分':>6}{'行情时间':>10}")
    lines.append(hdr)
    lines.append("-" * len(hdr))
    for r in results:
        vwap_s = f"{r['vwap_above_ratio']*100:.0f}%" if r.get('vwap_above_ratio') is not None else "N/A"
        vr_s = f"{r['vol_ratio']:.2f}" if r.get('vol_ratio') is not None else "N/A"
        score = r.get('signal_score', 0)
        score_s = f"{score:.0%}" if score is not None else "N/A"
        # 得分 ≥60% 标红高亮
        if score >= 0.6:
            score_s = f"\033[1;33m{score_s}\033[0m"
        lines.append(f"{r['code']:<10}{r['price']:>8.2f}{r['gain_pct']:>8.2f}{vr_s:>7}"
                     f"{r['turnover_pct']:>8.2f}{r['cap_yi']:>10.2f}{vwap_s:>8}"
                     f"{score_s:>6}{r['quote_time']:>10}")

    # 增强信号详情
    lines.append("")
    lines.append("--- 增强信号详情 ---")
    for r in results:
        enh = r.get("enhancements", {})
        parts = []
        tm = enh.get("tail_mom")
        if tm:
            cos_s = f"cos={tm['cos']:+.3f}" if tm.get("cos") is not None else "cos=N/A"
            ar_s = f"ar={tm['ar']:+.4f}" if tm.get("ar") is not None else "ar=N/A"
            parts.append(f"尾盘动量({cos_s},{ar_s},{tm['days']}日)")
        if "vwema_diverging" in enh:
            tag = "向上发散" if enh["vwema_diverging"] else "未发散"
            parts.append(f"VWEMA{tag}({enh.get('tail_vwema', '')})")
        vh = enh.get("vol_high")
        if vh:
            tag = "新高" if vh["is_high"] else "未新高"
            parts.append(f"量能{tag}({vh['avg_mult']:.2f}×均额)")
        if "breakout_quality" in enh:
            parts.append(f"突破{'真' if enh['breakout_quality'] else '假'}"
                         f"(收盘高比{enh['close_high_ratio']:.2f},放量{'是' if enh['vol_expand'] else '否'})")
        if "turnover_stable" in enh:
            parts.append(f"换手{'稳' if enh['turnover_stable'] else '过松'}"
                         f"(2日均{enh['turnover_2d_avg']:.1f}%)")
        if "rps" in enh:
            rps_str = ",".join(f"{p}日={v}" for p, v in enh["rps"].items())
            parts.append(f"RPS({rps_str})")
        if "ma_bullish_align" in enh:
            parts.append(f"多头排列{'是' if enh['ma_bullish_align'] else '否'}")
        if "ma_position" in enh:
            ma_str = ",".join(
                f"{p}日{'上↑' if i['above'] and i['rising'] else ('上→' if i['above'] else '下')}"
                for p, i in enh["ma_position"].items()
            )
            parts.append(f"MA({ma_str})")
        if "cpr" in enh:
            c = enh["cpr"]
            parts.append(f"CPR(上{c['upper']}/中{c['pivot']}/下{c['lower']})")
        if parts:
            lines.append(f"  {r['code']}: {' | '.join(parts)}")

    lines.append("")
    lines.append("⚔ 铁的纪律: 次日早盘 (10:30 前) 无论盈亏必须清仓，持股不超 4 小时。")
    lines.append("📊 得分 = 增强信号命中数 / 信号总数，≥60% 为优选标的。")
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
# 主流程
# ---------------------------------------------------------------------------
def main():
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
    conn.execute("USE tdx")

    # 标的池
    if args.code:
        market, code = parse_code(args.code)
        universe = [(market, code)]
    elif args.all:
        universe = all_mainboard_codes(conn)
    else:
        universe = [(parse_code(c)[0], parse_code(c)[1]) for c in zxg_codes()]

    # 预过滤
    if not args.code:
        shm_codes = set(shm.all_quotes().keys())
        filtered = []
        for m, c in universe:
            if c not in shm_codes:
                continue
            if table_exists(conn, f"k_{m}{c}_1d"):
                filtered.append((m, c))
        universe = filtered
        sys.stderr.write(f"[scalper] 标的池: {len(universe)} 只\n")

    require_intraday = not args.no_intraday
    round_no = 0
    start_epoch = time.time()

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

            # 执行一轮筛选
            round_no += 1
            t0 = time.time()
            sys.stderr.write(f"[scalper] 第 {round_no} 轮筛选开始...\n")
            try:
                results = screen(conn, universe, shm, cfg,
                                 require_intraday=require_intraday)
                elapsed = time.time() - t0
                output = render_table(results, round_no, elapsed)
                sys.stdout.write(output)
                sys.stdout.flush()
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