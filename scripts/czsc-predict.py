#!/usr/bin/env python3
"""一键预测脚本：为每只股票生成缠论趋势质量评估报告（1d/30m/5m）

基于 C++ czsc 引擎（tdx czsc-structure），不再依赖 Python czsc / talib。

用法:
  python3 scripts/czsc-predict.py                      # 从TDX自选股读取
  python3 scripts/czsc-predict.py 600519.SH 999999.SH  # 手动指定
  python3 scripts/czsc-predict.py -n 4 600519.SH 999999.SH  # 指定并发数

输出:
  自选股模式 → output/czsc-zxg-yyyymmdd.md
  手动模式   → output/czsc-<symbol>.md
"""
import argparse
import json
import logging
import os
import subprocess
import sys
import time
import warnings
from concurrent.futures import ThreadPoolExecutor, as_completed
from datetime import date, timedelta
from pathlib import Path

warnings.filterwarnings("ignore", category=FutureWarning)

from taosws import connect

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TDX_BIN = os.environ.get("TDX_BIN", os.path.join(REPO, "build", "bin", "tdx"))
TDENGINE_URL = os.environ.get("TDENGINE_URL", "taosws://root:taosdata@localhost:6041/tdx")

FREQS = [("1d", "1d"), ("30m", "30m"), ("5m", "5m")]

# 加速度阈值：不同周期价格波动幅度不同，用标准化阈值（‰）
ACCEL_THRESHOLDS = {
    "1d": (0.3, -0.3),
    "30m": (1.0, -1.0),
    "5m": (3.0, -3.0),
}


def _setup_logging(log_file: str) -> logging.Logger:
    """配置日志：文件记录详细信息，屏幕只显示进度"""
    logger = logging.getLogger("predict")
    logger.setLevel(logging.DEBUG)
    logger.handlers.clear()
    fh = logging.FileHandler(log_file, encoding="utf-8")
    fh.setLevel(logging.DEBUG)
    fh.setFormatter(logging.Formatter("%(asctime)s %(levelname)s %(message)s"))
    logger.addHandler(fh)
    return logger


# ---- 代码解析（600519.SH → sh600519） ----
def _strip_suffix(symbol: str) -> str:
    s = symbol.upper()
    for suf in (".SH", ".SZ", ".BJ", ".HK"):
        if s.endswith(suf):
            return s[:-3]
    return s


def _market_of(symbol: str) -> str:
    s = symbol.upper()
    if s.endswith(".SZ"):
        return "sz"
    if s.endswith(".BJ"):
        return "bj"
    if s.endswith(".HK"):
        return "hk"
    return "sh"


def _td_code(symbol: str) -> str:
    return _market_of(symbol) + _strip_suffix(symbol)


def _batch_stock_names(symbols: list[str]) -> dict[str, str]:
    """从 TDengine stock_name 表批量获取股票名称"""
    name_map: dict[str, str] = {}
    raw_codes = list({_strip_suffix(s) for s in symbols})
    try:
        conn = connect()
        conn.query("USE tdx")
    except Exception:
        return {s: s for s in symbols}
    try:
        placeholders = ",".join(f"'{c}'" for c in raw_codes)
        r = conn.query(
            f"select code, name, market from tdx.stock_name where code in ({placeholders})"
        )
        precise: dict[tuple[str, str], str] = {}
        by_code: dict[str, str] = {}
        for row in r:
            precise[(row[0], row[2])] = row[1]
            by_code.setdefault(row[0], row[1])
    finally:
        conn.close()
    for symbol in symbols:
        raw = _strip_suffix(symbol)
        name = precise.get((raw, _market_of(symbol))) or by_code.get(raw) or symbol
        name_map[symbol] = name
    return name_map


# ---- 缠论结构获取（调 C++ czsc-structure） ----
def _get_structure(code: str, freq: str) -> dict | None:
    """调 tdx czsc-structure <code> <freq> 获取缠论结构 JSON。失败返回 None。"""
    try:
        r = subprocess.run([TDX_BIN, "czsc-structure", code, freq],
                           capture_output=True, text=True, timeout=60)
        if r.returncode != 0:
            return None
        d = json.loads(r.stdout)
        return d if d.get("bi_count", 0) > 0 else None
    except Exception:
        return None


# ---- 分型强度（读 JSON elements 算影线） ----
def _fx_enhanced_power(fx: dict) -> str:
    """增强分型强度判定：根据影线 + 成交量组合决定强度"""
    if not fx:
        return "-"
    power = fx.get("power", "中")
    is_ding = fx.get("mark") == 0  # Mark::kD=0=顶分型, kG=1=底分型（对齐 Rust czsc）
    # 从 elements 中间 K 线算影线
    elems = fx.get("elements", [])
    if len(elems) < 3:
        return power
    mid = elems[len(elems) // 2]
    prev = elems[0]
    nxt = elems[-1]
    tags = []
    total_range = mid["h"] - mid["l"]
    if total_range <= 0:
        return power
    upper_shadow = mid["h"] - max(mid["c"], mid["o"])
    lower_shadow = min(mid["c"], mid["o"]) - mid["l"]
    upper_pct = upper_shadow / total_range * 100
    lower_pct = lower_shadow / total_range * 100
    has_long_shadow = False
    if is_ding and upper_pct >= 40:
        tags.append("长上影")
        has_long_shadow = True
    elif not is_ding and lower_pct >= 40:
        tags.append("长下影")
        has_long_shadow = True
    avg_vol = (prev["v"] + nxt["v"]) / 2 if (prev["v"] + nxt["v"]) > 0 else 1
    vol_ratio = mid["v"] / avg_vol if avg_vol > 0 else 1
    if vol_ratio >= 1.5:
        tags.append("放量")
    elif vol_ratio <= 0.6:
        tags.append("缩量")
    if has_long_shadow:
        power = "强" if vol_ratio >= 1.5 else "中"
    tags.insert(0, power)
    return "|".join(tags) if len(tags) > 1 else power


# ---- MACD/CCI 状态（读 C++ 输出序列） ----
def _find_divergence(series, closes, lookback=80):
    """检测 series 与价格的顶/底背离"""
    n = len(series)
    start = max(0, n - lookback)
    peaks, troughs = [], []
    for i in range(start + 1, n - 1):
        v = series[i]
        if v is None or (isinstance(v, float) and v != v):
            continue
        if v > series[i - 1] and v > series[i + 1]:
            peaks.append((i, v, closes[i]))
        if v < series[i - 1] and v < series[i + 1]:
            troughs.append((i, v, closes[i]))
    if len(peaks) >= 2 and peaks[-1][2] > peaks[-2][2] and peaks[-1][1] < peaks[-2][1]:
        return "顶背离"
    if len(troughs) >= 2 and troughs[-1][2] < troughs[-2][2] and troughs[-1][1] > troughs[-2][1]:
        return "底背离"
    return ""


def _macd_cci_status(macd: dict, cci: list, closes: list) -> tuple[str, str]:
    """计算 MACD 和 CCI 当前状态。macd/cci/closes 为 C++ 输出序列。"""
    dif, dea, hist = macd["dif"], macd["dea"], macd["hist"]

    # ── MACD ──
    macd_text = "-"
    if len(hist) >= 2:
        h_cur, h_prev = hist[-1], hist[-2]
        m, s = dif[-1], dea[-1]
        if h_cur > h_prev:
            strength = "上涨动能增强" if h_cur > 0 else "下跌动能衰竭"
        else:
            strength = "上涨动能衰竭" if h_cur > 0 else "下跌动能增强"
        if m > s and len(dif) >= 2 and dif[-2] <= dea[-2]:
            bar = "金叉"
        elif m < s and len(dif) >= 2 and dif[-2] >= dea[-2]:
            bar = "死叉"
        elif h_cur > 0 and h_cur > h_prev:
            bar = "红柱放大"
        elif h_cur > 0:
            bar = "红柱缩小"
        elif h_cur < 0 and h_cur < h_prev:
            bar = "绿柱放大"
        else:
            bar = "绿柱缩小"
        macd_text = f"{bar} {strength}"
        div = _find_divergence(list(hist), list(closes))
        if div:
            macd_text += f" {div}"

    # ── CCI(14) ──
    cci_text = "-"
    if len(cci) >= 2:
        cci_val = float(cci[-1])
        cci_prev = float(cci[-2])
        if cci_val == cci_val:  # not NaN
            cci_rising = cci_val > cci_prev
            if cci_val > 100:
                strength = "上涨动能增强" if cci_rising else "上涨动能衰竭"
            elif cci_val < -100:
                strength = "下跌动能增强" if not cci_rising else "下跌动能衰竭"
            elif cci_val > 0:
                strength = "偏多"
            else:
                strength = "偏空"
            cci_text = f"{cci_val:.0f} {strength}"
            div = _find_divergence(list(cci), list(closes))
            if div:
                cci_text += f" {div}"
    return macd_text, cci_text


# ---- 趋势评估（解析 C++ 结构 JSON） ----
def _zs_from_bis(bi_list: list[dict]) -> dict | None:
    """简化中枢：最近三笔重叠区 [zd, zg]。"""
    if len(bi_list) < 3:
        return None
    b3 = bi_list[-3:]
    zd = max(b["low"] for b in b3)
    zg = min(b["high"] for b in b3)
    if zg >= zd:
        return {"zd": zd, "zg": zg}
    return None


def assess_trend(struct: dict, freq_label: str = "1d") -> dict | None:
    """从缠论结构 JSON 提取趋势质量评估"""
    bi_list = struct.get("bi_list", [])
    if not bi_list:
        return None
    last_bi = bi_list[-1]

    cur_rsq = last_bi.get("rsq", 0)
    if cur_rsq > 0.8:
        rsq_msg = f"🟢 趋势规整 (R²={cur_rsq:.3f})<br>方向明确"
    elif cur_rsq > 0.6:
        rsq_msg = f"🟡 趋势一般 (R²={cur_rsq:.3f})<br>关注方向变化"
    else:
        rsq_msg = f"🔴 趋势散乱 (R²={cur_rsq:.3f})<br>方向不确定"

    accel = last_bi.get("acceleration", 0)
    bi_avg_price = (last_bi.get("high", 0) + last_bi.get("low", 0)) / 2 if last_bi.get("high", 0) > 0 else 1
    accel_norm = accel / bi_avg_price * 1000
    up_th, dn_th = ACCEL_THRESHOLDS.get(freq_label, (0.3, -0.3))
    if accel_norm > up_th:
        accel_msg = f"🟢 加速中 ({accel_norm:.2f}‰)<br>趋势强劲"
    elif accel_norm > dn_th:
        accel_msg = f"🟡 匀速/减速 ({accel_norm:.2f}‰)<br>关注转折"
    else:
        accel_msg = f"🔴 反向加速 ({accel_norm:.2f}‰)<br>趋势可能反转"

    same_dir = [b for b in bi_list if b["direction"] == last_bi["direction"]]
    if len(same_dir) >= 3:
        powers = [b["power"] for b in same_dir[-3:]]
        if powers[-1] < powers[0] * 0.5:
            power_msg = "🔴 同向笔力度衰减 > 50%，动能衰竭"
        elif powers[-1] < powers[0] * 0.7:
            power_msg = "🟡 同向笔力度递减中，注意动能不足"
        else:
            power_msg = "🟢 力度稳定，趋势健康"
    else:
        power_msg = "⚪ 同向笔不足 3 根，无法评估力度变化"

    direction = "上升笔 📈" if last_bi["direction"] == "向上" else "下降笔 📉"

    # 未完成笔信息
    ubi_info = ""
    ubi_bar_count = 0
    bars_ubi = struct.get("bars_ubi", [])
    if bars_ubi:
        ubi_bar_count = len(bars_ubi)
        ubi_up = last_bi["direction"] == "向下"  # 完成笔向下 → ubi 向上
        ubi_dir = "↑ 向上" if ubi_up else "↓ 向下"
        first_close = bars_ubi[0]["c"]
        last_close = bars_ubi[-1]["c"]
        cur_price = last_close
        power_val = last_close - first_close
        price_pct = power_val / first_close * 100 if first_close > 0 else 0

        # 分型（增强）
        ubi_fxs = struct.get("ubi_fxs", [])
        fx_text = "-"
        last_fx_power = "-"
        if ubi_fxs:
            last_fx = ubi_fxs[-1]
            fx_mark = "顶分型" if last_fx.get("mark") == 0 else "底分型"
            last_fx_power = _fx_enhanced_power(last_fx)
            fx_text = f"{fx_mark}({last_fx_power})"

        # 中枢
        zs_text = "-"
        zs = _zs_from_bis(bi_list)
        if zs:
            if zs["zd"] <= cur_price <= zs["zg"]:
                zs_text = f"{zs['zd']:.2f}-{zs['zg']:.2f}"
            elif cur_price > zs["zg"]:
                zs_text = f"上方 {zs['zg']:.2f}"
            else:
                zs_text = f"下方 {zs['zd']:.2f}"

        # 买卖点
        bs_text = "-"
        if zs:
            if ubi_up:
                if cur_price < zs["zd"]:
                    bs_text = f"一买（{last_fx_power}）"
                elif cur_price <= zs["zg"]:
                    bs_text = f"二买（{last_fx_power}）"
                else:
                    bs_text = f"三买（{last_fx_power}）"
            else:
                if cur_price > zs["zg"]:
                    bs_text = f"一卖（{last_fx_power}）"
                elif cur_price >= zs["zd"]:
                    bs_text = f"二卖（{last_fx_power}）"
                else:
                    bs_text = f"三卖（{last_fx_power}）"

        # MACD/CCI（C++ 已算序列）
        macd_text, cci_text = _macd_cci_status(
            struct.get("macd", {}), struct.get("cci", []),
            [b["c"] for b in struct.get("bars_raw", [])])

        ubi_info = (
            f"未完成笔 ({ubi_bar_count} 根K线)：{ubi_dir}<br>"
            f"起始：{_fmt_dt(bars_ubi[0]['dt'])}<br>"
            f"力度：{power_val:+.2f} ({price_pct:+.1f}%)<br>"
            f"分型：{fx_text}<br>"
            f"中枢：{zs_text}<br>"
            f"买卖点：{bs_text}<br>"
            f"MACD：{macd_text}<br>"
            f"CCI：{cci_text}"
        )

    return {
        "rsq_msg": rsq_msg,
        "accel_msg": accel_msg,
        "power_msg": power_msg,
        "direction": direction,
        "ubi_info": ubi_info,
        "ubi_bar_count": ubi_bar_count,
        "last_bi_power": last_bi.get("power", 0),
        "bi_count": len(bi_list),
        "bar_count": struct.get("bars_raw_count", 0),
    }


def _fmt_dt(ts):
    """epoch 秒 → YYYY-MM-DD"""
    from datetime import datetime
    return datetime.fromtimestamp(ts).strftime("%Y-%m-%d")


# ---- 单只预测 ----
def predict_stock(symbol, sdt, edt, logger=None):
    """对单只股票生成多周期预测结果"""
    code = _td_code(symbol)
    results = {}
    for label, freq in FREQS:
        try:
            struct = _get_structure(code, freq)
            if struct is None:
                results[label] = {"error": "无数据或结构不足"}
                continue
            trend = assess_trend(struct, freq_label=label)
            if trend is None:
                results[label] = {"error": "未检测到笔"}
            else:
                results[label] = trend
        except Exception as e:
            if logger:
                logger.error(f"{symbol} {label} 分析失败: {e}")
            results[label] = {"error": str(e)}
    return results


# ---- 综合信号 / 星级（保留原逻辑） ----
def _overall_signal(results):
    ups = downs = 0
    for label, _ in FREQS:
        r = results.get(label)
        if r and "error" not in r and r.get("direction"):
            if "上升" in r["direction"]:
                ups += 1
            else:
                downs += 1
    if ups + downs == 0:
        return "⚪ 数据不足，无法判断"
    if ups > downs:
        return f"🟢 偏多 ({ups}↑ {downs}↓)"
    elif downs > ups:
        return f"🔴 偏空 ({ups}↑ {downs}↓)"
    return f"🟡 多空均衡 ({ups}↑ {downs}↓)"


def _extract_bs_from_ubi(ubi_info):
    if not ubi_info:
        return ""
    for prefix in ("买卖点：", "买卖点:"):
        if prefix in ubi_info:
            return ubi_info.split(prefix)[-1].split("<br>")[0]
    return ""


def _position_alert(results):
    for label in ("1d", "30m", "5m"):
        r = results.get(label)
        if not r or "error" in r:
            return "", ""
    d_bs = _extract_bs_from_ubi(results["1d"]["ubi_info"])
    m30_bs = _extract_bs_from_ubi(results["30m"]["ubi_info"])
    m5_bs = _extract_bs_from_ubi(results["5m"]["ubi_info"])
    all_buy = all("买" in bs for bs in [d_bs, m30_bs, m5_bs])
    all_sell = all("卖" in bs for bs in [d_bs, m30_bs, m5_bs])
    if all_buy:
        pts = "、".join(bs.split("（")[0] for bs in [d_bs, m30_bs, m5_bs])
        return f"🔺 加仓：三周期共振买点（{pts}）", "add"
    if all_sell:
        pts = "、".join(bs.split("（")[0] for bs in [d_bs, m30_bs, m5_bs])
        return f"🔻 减仓：三周期共振卖点（{pts}）", "reduce"
    return "", ""


def _strong_fx_bs_star(results):
    required = 0
    for label in ("1d", "30m", "5m"):
        r = results.get(label)
        if not r or "error" in r:
            return ""
        ubi = r.get("ubi_info", "")
        if ("强)" in ubi or "强|" in ubi) and ("买" in ubi or "卖" in ubi):
            required += 1
    if required >= 3:
        return '<span style="color:orange">⭐⭐</span> '
    if required >= 2:
        return '<span style="color:orange">⭐</span> '
    return ""


def _ubi_star(results):
    daily = results.get("1d")
    m30 = results.get("30m")
    m5 = results.get("5m")
    if not all([daily, m30, m5]):
        return ""
    if any("error" in r for r in [daily, m30, m5]):
        return ""
    daily_ubi_up = "下降" in daily["direction"]
    m30_ubi_up = "下降" in m30["direction"]
    m5_ubi_up = "下降" in m5["direction"]
    if daily_ubi_up and m30_ubi_up and m5_ubi_up:
        return '<span style="color:red">★★</span> '
    if not daily_ubi_up and m30_ubi_up and m5_ubi_up:
        return '<span style="color:red">★</span> '
    return ""


# ---- 报告生成（保留原逻辑） ----
def format_md(symbol, results, name=None):
    star = _ubi_star(results)
    fx_star = _strong_fx_bs_star(results)
    label = f"{name}（{symbol}）" if name else symbol
    lines = []
    markers = f"{star}{fx_star}".strip()
    lines.append(f"<h1>{markers} {label} 缠论趋势预测</h1>" if markers else f"# {label} 缠论趋势预测")
    lines.append("")
    lines.append("## 趋势质量评估")
    lines.append("")
    labels = [l for l, _ in FREQS]
    rows = {"之前趋势": [], "趋势规整度": [], "加速度": [], "力度评估": [], "未完成笔": []}
    for label, _ in FREQS:
        r = results.get(label)
        if r and "error" not in r:
            ubi = r["ubi_info"] if r["ubi_info"] else "无"
            dir_with_power = f"{r['direction']}<br>力度={r['last_bi_power']:.1f}"
            rows["未完成笔"].append(ubi)
            rows["之前趋势"].append(dir_with_power)
            rows["趋势规整度"].append(r["rsq_msg"])
            rows["加速度"].append(r["accel_msg"])
            rows["力度评估"].append(r["power_msg"])
        else:
            err = r.get("error", "数据获取失败") if r else "未知错误"
            rows["未完成笔"].append(f"⚠️ {err}")
            rows["之前趋势"].append("-")
            rows["趋势规整度"].append("-")
            rows["加速度"].append("-")
            rows["力度评估"].append("-")
    alert_text, _ = _position_alert(results)
    if alert_text:
        alert_line = f"<br>{alert_text}"
        rows["未完成笔"] = [v + alert_line for v in rows["未完成笔"]]
    lines.append('<table style="border-collapse:collapse">')
    lines.append('<colgroup><col style="width:8em;white-space:nowrap;text-align:center">')
    lines.append("".join('<col style="text-align:left">' for _ in labels))
    lines.append("</colgroup>")
    lines.append('<tr><th style="text-align:center">指标</th>' + "".join(f"<th>{l}</th>" for l in labels) + "</tr>")
    for indicator, values in rows.items():
        lines.append('<tr><td style="text-align:center">' + indicator + "</td>" + "".join(f"<td>{v}</td>" for v in values) + "</tr>")
    lines.append("</table>")
    lines.append("")
    return "\n".join(lines)


TDX_ZXG_PATH = Path("/home/li/.local/share/tdxcfv/drive_c/tc/T0002/blocknew/zxg.blk")


def _parse_tdx_blk(path):
    market_map = {"1": "SH", "0": "SZ"}
    _tdx_code_remap = {"000001.SH": "999999.SH"}
    symbols = []
    with open(path, encoding="gbk", errors="ignore") as f:
        for line in f:
            code = line.strip()
            if len(code) == 7 and code[0] in market_map:
                s = f"{code[1:]}.{market_map[code[0]]}"
                symbols.append(_tdx_code_remap.get(s, s))
    return symbols


def _merged_filename(symbols):
    parts = [s.replace(".", "_") for s in symbols]
    return os.path.join(REPO, "output", f"czsc-{'_'.join(parts)}.md")


def _sort_key(symbol, all_results, name_map):
    name = (name_map or {}).get(symbol, "")
    signal = _overall_signal(all_results[symbol])
    alert, _ = _position_alert(all_results[symbol])
    fx_star = _strong_fx_bs_star(all_results[symbol])
    if "上证指数" in name:
        return 0
    if "创业板指" in name:
        return 1
    if "⭐⭐" in fx_star:
        return 2
    if alert and "加仓" in alert:
        return 3
    if "⭐" in fx_star:
        return 4
    if "偏多" in signal:
        return 5
    if "多空均衡" in signal:
        return 6
    if "偏空" in signal:
        return 7
    return 8


def _write_merged_report(symbols, all_results, filename, name_map=None):
    symbols = sorted(symbols, key=lambda s: _sort_key(s, all_results, name_map))
    lines = [f"# 缠论趋势预测报告（{len(symbols)}只股票）", ""]
    for symbol in symbols:
        n = (name_map or {}).get(symbol)
        md = format_md(symbol, all_results[symbol], name=n)
        lines.append(md)
        lines.append("")
    with open(filename, "w", encoding="utf-8") as f:
        f.write("\n".join(lines))


def main():
    parser = argparse.ArgumentParser(description="缠论趋势预测（C++ czsc 引擎）")
    parser.add_argument("symbols", nargs="*", help="股票代码，如 600519.SH 999999.SH")
    parser.add_argument("-n", "--workers", type=int, default=16, help="并行线程数（默认 16）")
    args = parser.parse_args()

    if args.symbols:
        symbols = args.symbols
        from_zxg = False
    elif TDX_ZXG_PATH.exists():
        symbols = _parse_tdx_blk(TDX_ZXG_PATH)
        if not symbols:
            sys.exit(f"TDX自选股文件为空: {TDX_ZXG_PATH}")
        from_zxg = True
    else:
        print("用法: python3 scripts/czsc-predict.py [股票代码1] [股票代码2] ...")
        sys.exit(1)

    edt = date.today().strftime("%Y-%m-%d")
    sdt = (date.today() - timedelta(days=365)).strftime("%Y-%m-%d")

    os.makedirs(os.path.join(REPO, "output"), exist_ok=True)
    log_file = os.path.join(REPO, "output", f"predict_{edt.replace('-', '')}.log")
    logger = _setup_logging(log_file)

    t_start = time.time()
    print(f"数据范围: {sdt} ~ {edt}")
    print(f"待预测({len(symbols)}): {', '.join(symbols)}")
    print(f"并发数: {args.workers}")
    print("=" * 60)

    print("获取股票名称...")
    name_map = _batch_stock_names(symbols)

    all_results = {}
    total = len(symbols)
    if total == 1:
        symbol = symbols[0]
        print(f"\n[1/1] {name_map.get(symbol, symbol)} ...")
        all_results[symbol] = predict_stock(symbol, sdt, edt, logger=logger)
    else:
        workers = min(args.workers, total)
        with ThreadPoolExecutor(max_workers=workers) as executor:
            futures = {executor.submit(predict_stock, symbol, sdt, edt, logger): symbol
                       for symbol in symbols}
            done_count = 0
            for future in as_completed(futures):
                symbol = futures[future]
                done_count += 1
                try:
                    all_results[symbol] = future.result()
                except Exception as e:
                    logger.error(f"{symbol} 分析异常: {e}")
                    all_results[symbol] = {label: {"error": str(e)} for label, _ in FREQS}
                print(f"  [{done_count}/{total}] {name_map.get(symbol, symbol)} 完成")

    if from_zxg:
        filename = os.path.join(REPO, "output", f"czsc-zxg-{edt.replace('-', '')}.md")
    elif len(symbols) == 1:
        filename = os.path.join(REPO, "output", f"czsc-{symbols[0].replace('.', '_')}.md")
    else:
        filename = _merged_filename(symbols)

    if len(symbols) == 1 and not from_zxg:
        md = format_md(symbols[0], all_results[symbols[0]], name=name_map.get(symbols[0]))
        with open(filename, "w", encoding="utf-8") as f:
            f.write(md)
    else:
        _write_merged_report(symbols, all_results, filename, name_map=name_map)

    print(f"\n  → {filename}")
    print(f"  → {log_file}")
    print(f"完成，耗时 {time.time() - t_start:.1f} 秒")


if __name__ == "__main__":
    main()
