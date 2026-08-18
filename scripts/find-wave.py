#!/usr/bin/env python3
"""
波浪分析 — 识别 K 线波峰波谷，量化涨跌力度。
直接查询 TDengine kline 表，不依赖 tdxdata/mootdx/opentdx。

周期支持：1/5/15/30/60m、1/2d、1w
原生表仅 1m/5m/1d；其它周期通过 TDengine INTERVAL 自动采样。

用法:
  python3 scripts/find-wave.py --code sh600276 --cycle 1d
  python3 scripts/find-wave.py --code 600276 --cycle 1d --threshold 5
  python3 scripts/find-wave.py --code sz000001 --cycle 1m --window 5 --json
  python3 scripts/find-wave.py --code sh999999 --cycle 60m        # 由 1m 采样
"""
import argparse
import json
import os
import sys
from datetime import datetime

import taosws

from common import parse_code  # 复用本仓代码解析（对齐 sh/sz/bj 前缀 + 裸码推断）

# ---- 配置 ----
TDENGINE_URL = os.environ.get("TDENGINE_URL", "taosws://root:taosdata@localhost:6041/tdx")
OUTPUT_DIR = os.environ.get("FIND_WAVE_DIR", os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "output", "find-wave"))

# 原生周期（由细到粗），仅这些表存在于 TDengine
NATIVE_CYCLES = ("1m", "5m", "1d")


def get_conn():
    return taosws.connect(TDENGINE_URL)


def table_name(market: str, code: str, cycle: str) -> str:
    return f"tdx.k_{market}{code}_{cycle}"


# ---- 表存在检查 ----
def _table_exists(conn, tbl: str) -> bool:
    """查询 information_schema 判断某子表是否存在。"""
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


def _find_native_source(conn, market: str, code: str, cycle: str) -> str | None:
    """
    当目标周期表不存在时，找出可用于采样的最细原生周期。
    返回值形如 "1m"、"5m"、"1d"，或 None（无法采样）。

    规则：
      - 分钟级目标（15m/30m/60m）→ 优先从 1m，不可用时用 5m
      - 日级目标（1d/2d/5d/1w 等）→ 从 1d
      - 目标本身是原生周期但以不同倍数出现的情况不会发生（已在上层处理）
    """
    # 规范化：将周期归到类型
    def _cycle_type(c):
        if c in ("1m", "5m") or c.endswith("m"):
            return "intraday"
        if c.endswith("d") or c.endswith("w") or c.endswith("mo"):
            return "daily"
        return "unknown"

    ctype = _cycle_type(cycle)
    if ctype == "intraday":
        for nc in ("1m", "5m"):
            if _table_exists(conn, f"k_{market}{code}_{nc}"):
                return nc
    elif ctype == "daily":
        if _table_exists(conn, f"k_{market}{code}_1d"):
            return "1d"
    return None


# ---- K 线查询 ----
def _row_to_bar(row) -> dict:
    """将 taosws 行转换为 bar dict，统一处理时间戳和空值。"""
    ts = row[0]
    return {
        "ts": ts.isoformat() if hasattr(ts, "isoformat") else str(ts),
        "open": float(row[1]) if row[1] is not None else 0.0,
        "high": float(row[2]) if row[2] is not None else 0.0,
        "low": float(row[3]) if row[3] is not None else 0.0,
        "close": float(row[4]) if row[4] is not None else 0.0,
        "volume": float(row[5]) if row[5] is not None else 0.0,
        "amount": float(row[6]) if row[6] is not None else 0.0,
    }


def _query_direct(conn, tbl: str, limit: int) -> list[dict]:
    sql = f"SELECT ts, open, high, low, close, volume, amount FROM {tbl} ORDER BY ts"
    if limit > 0:
        sql += f" LIMIT {limit}"
    r = conn.query(sql)
    return [_row_to_bar(row) for row in r]


def _query_resampled(conn, native_tbl: str, target_cycle: str, limit: int) -> list[dict]:
    """通过 TDengine INTERVAL 把更细周期的表聚合为目标周期。"""
    sql = (
        f"SELECT FIRST(ts) AS ts, FIRST(open) AS open, MAX(high) AS high, "
        f"MIN(low) AS low, LAST(close) AS close, SUM(volume) AS volume, SUM(amount) AS amount "
        f"FROM {native_tbl} INTERVAL({target_cycle})"
    )
    r = conn.query(sql)
    bars = [_row_to_bar(row) for row in r]
    if limit > 0:
        bars = bars[-limit:]
    return bars


def query_kline(
    conn, market: str, code: str, cycle: str, limit: int = 0
) -> tuple[list[dict], str]:
    """
    查询 K 线数据。

    Returns:
        (bars, source_desc)
        source_desc: "原生" 或 "↪<源周期>"（表示由某细周期采样合成）。
    """
    tbl = f"k_{market}{code}_{cycle}"

    # 目标表存在 → 直接查
    if _table_exists(conn, tbl):
        bars = _query_direct(conn, table_name(market, code, cycle), limit)
        return bars, "原生"

    # 不存在 → 尝试从更细原生周期采样
    source = _find_native_source(conn, market, code, cycle)
    if source is None:
        raise RuntimeError(
            f"周期表 tdx.{tbl} 不存在，且无可用的更细周期用于采样。"
            f"本机原生周期: {list(NATIVE_CYCLES)}"
        )

    source_tbl = table_name(market, code, source)
    bars = _query_resampled(conn, source_tbl, cycle, limit)
    if not bars:
        raise RuntimeError(
            f"从 {source} 采样 {cycle} 失败：{source_tbl} 无数据或时间范围不足以合成任何 K 线"
        )
    return bars, f"↪{source}"


# ---- 波浪检测 ----
def detect_waves(bars: list[dict], window: int = 2, min_reversal_pct: float = 3.0) -> dict | None:
    """
    检测波峰和波谷。

    Args:
        bars: K 线列表，按时间升序
        window: 局部极值窗口（左右各 N 根 bar）
        min_reversal_pct: 确认反转的最小涨跌幅（%）

    Returns:
        {extremes: [...], up_waves: [...], down_waves: [...], summary: {...}}
    """
    n = len(bars)
    if n < 2 * window + 3:
        return None

    # Step 1: 找局部极值候选
    candidates = []
    for i in range(window, n - window):
        left = i - window
        right = i + window + 1

        # 局部高点
        if bars[i]["high"] == max(bars[j]["high"] for j in range(left, right)):
            # 确保不全是平顶（至少严格大于一侧）
            left_max = max(bars[j]["high"] for j in range(left, i))
            right_max = max(bars[j]["high"] for j in range(i + 1, right))
            if bars[i]["high"] > left_max or bars[i]["high"] > right_max:
                candidates.append({"idx": i, "type": "peak", **bars[i]})

        # 局部低点
        if bars[i]["low"] == min(bars[j]["low"] for j in range(left, right)):
            left_min = min(bars[j]["low"] for j in range(left, i))
            right_min = min(bars[j]["low"] for j in range(i + 1, right))
            if bars[i]["low"] < left_min or bars[i]["low"] < right_min:
                candidates.append({"idx": i, "type": "trough", **bars[i]})

    if len(candidates) < 2:
        return None

    # Step 2: 过滤为交替序列（同向连续出现取更极值，异向须满足反转阈值+方向一致）
    extremes = [candidates[0]]
    for c in candidates[1:]:
        last = extremes[-1]
        if c["type"] == last["type"]:
            if c["type"] == "peak" and c["high"] > last["high"]:
                extremes[-1] = c
            elif c["type"] == "trough" and c["low"] < last["low"]:
                extremes[-1] = c
        else:
            change_pct = abs(c["close"] - last["close"]) / last["close"] * 100
            if change_pct < min_reversal_pct:
                continue
            # 方向校验：波谷 close ≤ 前波峰 close，波峰 close ≥ 前波谷 close
            if c["type"] == "trough" and c["close"] > last["close"]:
                continue
            if c["type"] == "peak" and c["close"] < last["close"]:
                continue
            extremes.append(c)

    if len(extremes) < 2:
        return None

    # Step 3: 构建波浪段
    up_waves = []
    down_waves = []

    for i in range(1, len(extremes)):
        prev = extremes[i - 1]
        curr = extremes[i]

        change = (curr["close"] - prev["close"]) / prev["close"] * 100
        vol_ratio = round(curr["volume"] / prev["volume"], 2) if prev["volume"] > 0 else None
        amt_ratio = round(curr["amount"] / prev["amount"], 2) if prev["amount"] > 0 else None

        seg = {
            "ts_begin": prev["ts"],
            "ts_end": curr["ts"],
            "close_begin": prev["close"],
            "close_end": curr["close"],
            "close_change_pct": round(change, 2),
            "vol_begin": int(prev["volume"]),
            "vol_end": int(curr["volume"]),
            "vol_ratio": vol_ratio,
            "amt_begin": int(prev["amount"]),
            "amt_end": int(curr["amount"]),
            "amt_ratio": amt_ratio,
        }

        if curr["type"] == "peak":
            seg["direction"] = "up"
            up_waves.append(seg)
        else:
            seg["direction"] = "down"
            down_waves.append(seg)

    return {
        "extremes": [
            {
                "type": e["type"],
                "datetime": e["ts"],
                "open": e["open"],
                "high": e["high"],
                "low": e["low"],
                "close": e["close"],
                "volume": int(e["volume"]),
                "amount": int(e["amount"]),
            }
            for e in extremes
        ],
        "up_waves": up_waves,
        "down_waves": down_waves,
        "summary": {
            "total_bars": n,
            "total_extremes": len(extremes),
            "up_count": len(up_waves),
            "down_count": len(down_waves),
            "params": {"window": window, "min_reversal_pct": min_reversal_pct},
        },
    }


# ---- 统计 ----
def stats(waves: dict) -> dict:
    """计算涨跌统计。"""
    up = waves["up_waves"]
    down = waves["down_waves"]

    def _avg(lst, key):
        vals = [w[key] for w in lst if w.get(key) is not None]
        return round(sum(vals) / len(vals), 2) if vals else 0

    return {
        "up": {
            "count": len(up),
            "avg_change_pct": _avg(up, "close_change_pct"),
            "max_change_pct": max((w["close_change_pct"] for w in up), default=0),
            "avg_vol_ratio": _avg(up, "vol_ratio"),
            "avg_amt_ratio": _avg(up, "amt_ratio"),
        },
        "down": {
            "count": len(down),
            "avg_change_pct": _avg(down, "close_change_pct"),
            "max_change_pct": min((w["close_change_pct"] for w in down), default=0),
            "avg_vol_ratio": _avg(down, "vol_ratio"),
            "avg_amt_ratio": _avg(down, "amt_ratio"),
        },
    }


# ---- 输出 ----
def _fmt_vol(v: int) -> str:
    """格式化成交量: 亿/万/原值."""
    if v >= 1e8:
        return f"{v / 1e8:.1f}亿"
    if v >= 1e4:
        return f"{v / 1e4:.0f}万"
    return str(int(v))


def _emit_report_lines(code: str, cycle: str, result: dict, source_desc: str,
                      years: int = 0) -> list[str]:
    """生成文本报告行（由近到远），返回字符串列表。"""
    lines: list[str] = []
    _p = lambda *a: lines.append(" ".join(str(x) for x in a))
    s = result["summary"]
    st = stats(result)
    extremes = result["extremes"]  # oldest → newest

    tag = "" if source_desc == "原生" else f"  [采样自 {source_desc[1:]} → {cycle}]"
    years_tag = f"  近{years}年" if years > 0 else ""
    _p()
    _p(f"{'=' * 95}")
    _p(f"🌊 {code}.{cycle}{tag}{years_tag} 波浪分析（由近到远）")
    _p(f"{'=' * 95}")
    _p(f"K线:{s['total_bars']}  极值点:{s['total_extremes']}  "
       f"上涨:{st['up']['count']}  下跌:{st['down']['count']}  "
       f"window={s['params']['window']}  threshold={s['params']['min_reversal_pct']}%")

    # 表头
    _p()
    _p(f"{'日期':<12s} {'类型':<8s} {'开盘':>8s} {'最高':>8s} {'最低':>8s} "
       f"{'收盘':>8s} {'成交量':>9s} {'涨跌幅':>8s} {'量变化':>7s} {'额变化%':>9s}")
    _p("-" * 98)

    for i in range(len(extremes) - 1, -1, -1):
        e = extremes[i]
        emoji = "🔴" if e["type"] == "peak" else "🟢"
        tn = "波峰" if e["type"] == "peak" else "波谷"
        dt = e["datetime"][:10]

        if i > 0:
            prev = extremes[i - 1]
            pct = (e["close"] - prev["close"]) / prev["close"] * 100
            vol_r = e["volume"] / prev["volume"] if prev["volume"] else 0
            amt_pct = (e["amount"] - prev["amount"]) / prev["amount"] * 100 if prev["amount"] else 0
            pct_s = f"{pct:+.2f}%"
            vol_s = f"{vol_r:.2f}x"
            amt_s = f"{amt_pct:+.0f}%"
        else:
            pct_s = "-"
            vol_s = "-"
            amt_s = "-"

        _p(f"{dt:<12s} {emoji}{tn:<6s} {e['open']:>8.2f} {e['high']:>8.2f} "
           f"{e['low']:>8.2f} {e['close']:>8.2f} {_fmt_vol(e['volume']):>9s} "
           f"{pct_s:>8s} {vol_s:>7s} {amt_s:>9s}")

    _p()
    _p(f"📈 上涨: 均{st['up']['avg_change_pct']:+.2f}%  "
       f"最大{st['up']['max_change_pct']:+.2f}%  均量比{st['up']['avg_vol_ratio']}x")
    _p(f"📉 下跌: 均{st['down']['avg_change_pct']:+.2f}%  "
       f"最大{st['down']['max_change_pct']:+.2f}%  均量比{st['down']['avg_amt_ratio']}x")
    return lines


def _emit_md(code: str, cycle: str, result: dict, source_desc: str,
             years: int = 0) -> str:
    """生成 Markdown 报告文本。"""
    s = result["summary"]
    st = stats(result)
    extremes = result["extremes"]  # oldest → newest

    tag = "" if source_desc == "原生" else f"（采样自 {source_desc[1:]} → {cycle}）"
    years_tag = f"（近 {years} 年）" if years > 0 else ""
    parts: list[str] = []
    parts.append(f"# 🌊 {code}.{cycle} 波浪分析 {tag}{years_tag}\n")
    parts.append(f"**生成时间**: {datetime.now().strftime('%Y-%m-%d %H:%M:%S')}\n")
    if years > 0:
        parts.append(f"**数据窗口**: 近 {years} 年（{extremes[0]['datetime'][:10]} → {extremes[-1]['datetime'][:10]}）\n")
    parts.append(f"| 指标 | 值 |")
    parts.append(f"|------|------|")
    parts.append(f"| K 线数 | {s['total_bars']} |")
    parts.append(f"| 极值点 | {s['total_extremes']} |")
    parts.append(f"| 上涨波 | {st['up']['count']} |")
    parts.append(f"| 下跌波 | {st['down']['count']} |")
    parts.append(f"| window | {s['params']['window']} |")
    parts.append(f"| threshold | {s['params']['min_reversal_pct']}% |")
    parts.append("")
    parts.append("## 极值点（由近到远）\n")
    parts.append("| 日期 | 类型 | 开盘 | 最高 | 最低 | 收盘 | 成交量 | 涨跌幅 | 量变化 | 额变化% |")
    parts.append("|------|------|------|------|------|------|--------|--------|--------|---------|")

    for i in range(len(extremes) - 1, -1, -1):
        e = extremes[i]
        tn = "🔴 波峰" if e["type"] == "peak" else "🟢 波谷"
        dt = e["datetime"][:10]

        if i > 0:
            prev = extremes[i - 1]
            pct = (e["close"] - prev["close"]) / prev["close"] * 100
            vol_r = e["volume"] / prev["volume"] if prev["volume"] else 0
            amt_pct = (e["amount"] - prev["amount"]) / prev["amount"] * 100 if prev["amount"] else 0
            pct_s = f"{pct:+.2f}%"
            vol_s = f"{vol_r:.2f}x"
            amt_s = f"{amt_pct:+.0f}%"
        else:
            pct_s = "-"
            vol_s = "-"
            amt_s = "-"

        parts.append(
            f"| {dt} | {tn} | {e['open']:.2f} | {e['high']:.2f} "
            f"| {e['low']:.2f} | {e['close']:.2f} | {_fmt_vol(e['volume'])} "
            f"| {pct_s} | {vol_s} | {amt_s} |"
        )

    parts.append("")
    parts.append("## 统计\n")
    parts.append(f"- 📈 **上涨**: 均 {st['up']['avg_change_pct']:+.2f}%，"
                 f"最大 {st['up']['max_change_pct']:+.2f}%，均量比 {st['up']['avg_vol_ratio']}x")
    parts.append(f"- 📉 **下跌**: 均 {st['down']['avg_change_pct']:+.2f}%，"
                 f"最大 {st['down']['max_change_pct']:+.2f}%，均量比 {st['down']['avg_amt_ratio']}x")
    parts.append("")
    return "\n".join(parts) + "\n"


def _parse_date(ts: str):
    """模糊解析日期字符串 → date(年,月,日)，兼容 '2026-07-10T...' / '2026-07-10'。"""
    d = ts[:10]
    try:
        return datetime.strptime(d, "%Y-%m-%d").date()
    except ValueError:
        return datetime.fromisoformat(ts).date()


def _filter_recent_years(bars: list[dict], years: int) -> list[dict]:
    """按最后 bar 的时间回推 years 年过滤。保留 1 根哨兵 bar（窗口外第一根），
    确保最老极值点仍有前驱可算涨跌幅；但因哨兵 bar 在窗口范围内会被检测排除，
    若不足 years 窗口时返回原数组。years<=0 返回原数组。"""
    if years <= 0 or not bars:
        return bars
    last_date = _parse_date(bars[-1]["ts"])
    cutoff = last_date.replace(year=last_date.year - years)

    # 找最早一根 ≤ cutoff 的 bar 作为哨兵
    guard_idx = 0
    for i, b in enumerate(bars):
        if _parse_date(b["ts"]) >= cutoff:
            guard_idx = max(0, i - 1)  # 保留前一根作哨兵
            break
    else:
        return bars  # 全部早于截止日 → 不过滤

    n_before = len(bars)
    filtered = bars[guard_idx:]
    return filtered


def _md_filename(code: str, last_ts: str) -> str:
    """根据代码和最后 K 线时间戳生成 md 文件名: <code>-<yyyymmdd>.md。"""
    # last_ts 形如 "2026-07-10T00:00:00" 或 "2026-07-10 09:30:00"
    date_part = last_ts[:10].replace("-", "")
    return os.path.join(OUTPUT_DIR, f"{code}-{date_part}.md")


def _write_md(filepath: str, content: str):
    """写入 md 文件，自动创建目录。"""
    os.makedirs(os.path.dirname(filepath) or ".", exist_ok=True)
    with open(filepath, "w", encoding="utf-8") as f:
        f.write(content)


def main():
    parser = argparse.ArgumentParser(description="波浪分析 — K 线波峰波谷识别")
    parser.add_argument("--code", required=True, help="股票代码 (sh600276 / 600276 / sz000001)")
    parser.add_argument("--cycle", default="1d",
                        help="K线周期 (1m/5m/15m/30m/60m/1d/2d/5d/1w，非原生周期自动采样)")
    parser.add_argument("--window", type=int, default=2, help="极值检测窗口 (默认2，日内建议3-5)")
    parser.add_argument("--threshold", type=float, default=3.0, help="最小反转阈值%% (默认3)")
    parser.add_argument("--limit", type=int, default=0, help="取最近N根K线 (0=全部)")
    parser.add_argument("--years", type=int, default=0,
                        help="仅分析最近N年数据 (0=全部, 默认3年: --years 3)")
    parser.add_argument("--json", action="store_true", help="JSON 输出")
    parser.add_argument("--json-full", action="store_true", help="JSON 输出（含完整极值点列表）")
    args = parser.parse_args()

    market, code = parse_code(args.code)

    conn = get_conn()
    try:
        bars, source_desc = query_kline(conn, market, code, args.cycle, args.limit)
        if not bars:
            print(f"❌ 无数据: {args.code}.{args.cycle}", file=sys.stderr)
            sys.exit(1)

        # 按年份过滤（保留哨兵 bar 确保最老极值点有前驱）
        bars = _filter_recent_years(bars, args.years)

        result = detect_waves(bars, window=args.window, min_reversal_pct=args.threshold)
        if result is None:
            print(f"❌ 未检测到有效波浪（bar 数={len(bars)}，尝试减小 --window 或 --threshold）",
                  file=sys.stderr)
            sys.exit(1)

        result["code"] = f"{market}{code}"
        result["cycle"] = args.cycle

        md_content: str
        if args.json or args.json_full:
            output = {
                "code": result["code"],
                "cycle": result["cycle"],
                "data_source": source_desc,
                "summary": result["summary"],
                "stats": stats(result),
                "up_waves": result["up_waves"],
                "down_waves": result["down_waves"],
            }
            if args.json_full:
                output["extremes"] = result["extremes"]
            stdout_text = json.dumps(output, ensure_ascii=False, indent=2) + "\n"
            md_content = f"# 🌊 {result['code']}.{result['cycle']} 波浪分析\n\n```json\n{stdout_text}```\n"
        else:
            stdout_text = "\n".join(_emit_report_lines(
                f"{market}{code}", args.cycle, result, source_desc, years=args.years)) + "\n"
            md_content = _emit_md(
                f"{market}{code}", args.cycle, result, source_desc, years=args.years)

        # stdout 输出
        sys.stdout.write(stdout_text)
        sys.stdout.flush()

        # md 文件输出（文件名含最后 K 线日期）
        last_ts = bars[-1]["ts"]
        md_path = _md_filename(result["code"], last_ts)
        _write_md(md_path, md_content)
        sys.stderr.write(f"\n✅ 已保存: {md_path}\n")

    finally:
        conn.close()


if __name__ == "__main__":
    main()
