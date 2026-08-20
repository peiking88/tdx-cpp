#!/usr/bin/env python3
"""
市场杠杆风险监测 — 读 TDengine + 纯 Python 计算。

零外部依赖（无 akshare/pandas/openpyxl），仅需 taosws。
融资余额由 fetch-margin.py 入库；本脚本只读 TDengine。

指标:
  杠杆 = 融资余额 / 流通市值（>4% 风险）
  拥挤度 = 成交额前5%个股占比（>=45% 风险）
  净买入趋势 = 融资余额差分 MA20 + 年初累计（断顶底）

用法:
  python3 scripts/leverage-risk.py                            # 上一交易日
  python3 scripts/leverage-risk.py --date 20260818             # 指定日期
  python3 scripts/leverage-risk.py --symbols sh600000,sz300750 # 个股
  python3 scripts/leverage-risk.py --threshold 5.0            # 自定义阈值
  python3 scripts/leverage-risk.py --json                     # JSON 输出
"""
import argparse
import json
import os
import subprocess
import sys
import unicodedata
from datetime import datetime, timedelta
from pathlib import Path

import taosws

DB = os.environ.get("TDENGINE_DB", "tdx")
TDENGINE_URL = os.environ.get("TDENGINE_URL", "taosws://root:taosdata@localhost:6041")
RISK_THRESHOLD = 4.0
CROWDING_THRESHOLD = 45.0
CROWDING_TOP_PCT = 0.05
NETBUY_MA_WINDOW = 20
NETBUY_LOOKBACK = 60
DEFAULT_SEGMENTS = ["创业板", "科创板", "沪深两市"]

# 流通市值参考（亿元）— 接口失败时回退
REFERENCE_MV = {"创业板": 144934.15, "科创板": 107381.71, "沪深两市": 948326.0}


def get_conn():
    conn = taosws.connect(TDENGINE_URL)
    conn.query(f"USE {DB}")  # ponytail: taosws 多语句只返回首个结果集，USE 须单独发
    return conn


def query_all(conn, sql):
    """执行查询，返回 [dict(...), ...]。"""
    r = conn.query(sql)
    cols = [d.name() for d in r.fields]
    return [dict(zip(cols, row)) for row in r]


def query_one(conn, sql):
    rows = query_all(conn, sql)
    return rows[0] if rows else None


def previous_trading_day(ref=None):
    d = (ref or datetime.now()) - timedelta(1)
    while d.weekday() >= 5:
        d -= timedelta(1)
    return d


def margin_complete(conn, date_str):
    """该日融资余额是否 sz+sh 双市场齐全。

    单市场日（深交所明细未发布时 fetch-margin 只入沪市）会让板块汇总缺深市、
    净买入差分出现砍半级假跳变。
    """
    ts = f"{date_str[:4]}-{date_str[4:6]}-{date_str[6:8]} 00:00:00"
    try:
        mk = query_all(conn, f"SELECT market FROM margin WHERE ts='{ts}' GROUP BY market")
    except Exception:  # margin 表不存在（首次使用）→ 触发自动补录建表
        return False
    return len(mk) >= 2


def ensure_margin(conn, date_str):
    """数据缺失/不完整时自动调 fetch-margin.py 补录该日（子进程隔离 akshare 依赖）。

    fetch-margin 按日 DELETE+INSERT 幂等；半份成功（深交所未发布）退出码仍为 0，
    故以补录后重查 margin_complete 为准，不看退出码。
    """
    script = Path(__file__).resolve().parent / "fetch-margin.py"
    print(f"[leverage-risk] {date_str} 融资余额缺失/不完整，自动调用 fetch-margin 补录",
          file=sys.stderr)
    try:
        subprocess.run([sys.executable, str(script), "--date", date_str],
                       timeout=180, check=False)
    except Exception as e:
        print(f"  [warn] fetch-margin 调用失败: {e}", file=sys.stderr)


def resolve_date(conn, date_str):
    """确定融资余额日期：显式 > 从上一交易日起向前找首个双市场齐全日。

    不完整的候选日先自动补录一次，仍缺则往前找。
    """
    if date_str:
        if not margin_complete(conn, date_str):
            ensure_margin(conn, date_str)
            if not margin_complete(conn, date_str):
                print(f"[leverage-risk] 警告: {date_str} 仅单市场融资余额，板块占比将失真",
                      file=sys.stderr)
        return date_str
    d = previous_trading_day()
    for _ in range(7):
        ds = d.strftime("%Y%m%d")
        if not margin_complete(conn, ds):
            ensure_margin(conn, ds)
        if margin_complete(conn, ds):
            return ds
        d -= timedelta(1)
    raise RuntimeError("近 7 日无完整融资余额数据（sz+sh），自动补录失败")


def board_of(code):
    """按代码前缀分类板块。"""
    c = code[2:] if code[:2] in ("sh", "sz", "bj") else code
    if c.startswith(("300", "301")):
        return "创业板"
    if c.startswith("688"):
        return "科创板"
    return "沪市主板" if c.startswith(("60", "9")) else "深市主板"


def fetch_margin_by_board(conn, date_str):
    """按板块汇总融资余额（元）。"""
    ts = f"{date_str[:4]}-{date_str[4:6]}-{date_str[6:8]} 00:00:00"
    rows = query_all(
        conn,
        f"SELECT market, code, margin_balance FROM margin WHERE ts='{ts}'"
    )
    boards = {"创业板": 0.0, "深市主板": 0.0, "科创板": 0.0, "沪市主板": 0.0}
    total = 0.0
    for r in rows:
        b = board_of(r["code"])
        v = float(r["margin_balance"] or 0)
        boards[b] = boards.get(b, 0.0) + v
        total += v
    return boards, total


def fetch_margin_total(conn, date_str):
    """沪深融资余额合计（元）。"""
    ts = f"{date_str[:4]}-{date_str[4:6]}-{date_str[6:8]} 00:00:00"
    row = query_one(conn, f"SELECT SUM(margin_balance) AS s FROM margin WHERE ts='{ts}'")
    return float(row["s"] or 0) if row else 0.0


def fetch_circulating_mv(conn, codes):
    """从 fn_ 表读流通股本 × kline 最新收盘价 → 流通市值（亿元）。"""
    out = {}
    for code in codes:
        prefixed = code[:2] in ("sh", "sz", "bj")
        market = code[:2] if prefixed else ("sh" if code.startswith(("6", "9")) else "sz")
        bare = code[2:] if prefixed else code
        try:
            row = query_one(conn, f"SELECT liutongguben FROM fn_{bare} LIMIT 1")
            shares = float(row["liutongguben"] or 0) if row else 0.0
            if shares <= 0:
                continue
            price_row = query_one(
                conn,
                f"SELECT close FROM k_{market}{bare}_1d ORDER BY ts DESC LIMIT 1"
            )
        except Exception:  # 表不存在（未导入/非 A 股）
            continue
        price = float(price_row["close"] or 0) if price_row else 0.0
        if price > 0:
            out[code] = shares * price / 1e8
    return out


def fetch_live_mv():
    """实时获取板块流通市值（亿元）— 交易所官方接口。失败返回 None。"""
    # 注：交易所公开 API 需特定 Header，此处用 REFERENCE_MV 兜底。
    # 可用 --xxx-mv 手动覆盖。
    return None


def is_index(market, code):
    """非个股标的：88 通达信板块、sh 000/999 指数段 + 5 基金段、sz 399/395 指数段 + 15/16 基金段。"""
    if code.startswith(("88", "899")):  # 通达信板块指数 / 北证指数
        return True
    if market == "sh":
        return code.startswith(("000", "999", "5"))
    if market == "sz":
        return code.startswith(("399", "395", "15", "16"))
    return False


def fetch_crowding(conn, date_str):
    """交易拥挤度：各板块成交额前5%个股占比。"""
    # 日 bar ts 为收盘时刻（15:00），用当日范围匹配；cycle='1d' 排除分钟线
    d = datetime.strptime(date_str, "%Y%m%d")
    day, nxt = d.strftime("%Y-%m-%d"), (d + timedelta(1)).strftime("%Y-%m-%d")
    rows = query_all(
        conn,
        f"SELECT market, code, amount FROM kline "
        f"WHERE ts >= '{day}' AND ts < '{nxt}' AND cycle='1d' AND amount > 0"
    )
    rows = [r for r in rows if not is_index(r["market"], r["code"])]
    if not rows:
        return None

    by_board = {}
    for r in rows:
        b = board_of(r["code"])
        by_board.setdefault(b, []).append(float(r["amount"]))

    # 全市场
    all_amounts = [float(r["amount"]) for r in rows]
    by_board["沪深两市"] = all_amounts

    result = {}
    for board, amounts in by_board.items():
        amounts.sort(reverse=True)
        total = sum(amounts)
        n = len(amounts)
        top_n = max(1, int(n * CROWDING_TOP_PCT + 0.5))
        top_sum = sum(amounts[:top_n])
        result[board] = {
            "ratio": round(top_sum / total * 100, 2) if total > 0 else 0.0,
            "top_n": top_n,
            "total_n": n,
            "top_amount_yi": round(top_sum / 1e8, 2),
            "total_amount_yi": round(total / 1e8, 2),
        }
    return result


def complete_daily_totals(rows):
    """[(ts, market, total)] → [(ts, sz+sh合计)]，丢弃缺任一市场的日子。

    单市场日（深交所明细未发布时只入了沪市）若计入差分，会造成砍半级假净卖出。
    """
    by_day = {}
    for r in rows:
        by_day.setdefault(str(r["ts"])[:10], {})[r["market"]] = float(r["total"])
    return [(d, v["sz"] + v["sh"]) for d, v in sorted(by_day.items()) if "sz" in v and "sh" in v]


def fetch_netbuy_trend(conn, ma_window=NETBUY_MA_WINDOW, lookback=NETBUY_LOOKBACK):
    """融资净买入趋势 → 阶段顶底信号。"""
    rows = query_all(
        conn,
        f"SELECT ts, market, SUM(margin_balance) AS total FROM margin "
        f"GROUP BY ts, market ORDER BY ts"
    )
    balances = complete_daily_totals(rows)
    if len(balances) < ma_window + 2:
        return None
    year = datetime.now().year
    ytd = [(ts, bal) for ts, bal in balances if ts >= f"{year}-01-01"]
    if len(ytd) < 2:
        return None

    netbuy_daily = []
    for i in range(1, len(balances)):
        netbuy_daily.append((balances[i][0], balances[i][1] - balances[i - 1][1]))

    # MA20
    ma_now = None
    ma_trough = None
    if len(netbuy_daily) >= ma_window:
        recent = netbuy_daily[-ma_window:]
        ma_now = sum(x[1] for x in recent) / len(recent)
        # 近 N 日 MA 低谷
        lookback_data = netbuy_daily[-lookback:] if len(netbuy_daily) >= lookback else netbuy_daily
        ma_trough = min(x[1] for x in lookback_data)

    cum_ytd = ytd[-1][1] - ytd[0][1]
    cum_max = max(balances[i][1] - ytd[0][1] for i in range(len(balances)) if balances[i][0] >= f"{year}-01-01") if ytd else cum_ytd

    pos_at_max = (cum_ytd / cum_max * 100) if cum_max > 0 else 0.0
    is_new_high = cum_max > 0 and cum_ytd >= cum_max * 0.99
    ma_rebounding = (ma_trough is not None and ma_trough < 0 and ma_now is not None and ma_now > ma_trough)

    if is_new_high or pos_at_max >= 99:
        signal = "偏顶"
    elif ma_rebounding and pos_at_max < 30:
        signal = "偏底"
    else:
        signal = "中性"

    return {
        "date": balances[-1][0][:10],
        "year": year,
        "ma_window": ma_window,
        "netbuy_ma": round(ma_now / 1e8, 2) if ma_now is not None else None,
        "ma_trough": round(ma_trough / 1e8, 2) if ma_trough is not None else None,
        "cum_ytd": round(cum_ytd / 1e8, 2),
        "cum_max": round(cum_max / 1e8, 2),
        "pos_at_max": round(pos_at_max, 1),
        "signal": signal,
    }


def analyze(conn, date_str, threshold, symbols=None, crowding_threshold=CROWDING_THRESHOLD):
    """执行杠杆风险分析。"""
    date_str = resolve_date(conn, date_str)
    board_margin, _ = fetch_margin_by_board(conn, date_str)
    total_margin = fetch_margin_total(conn, date_str)

    # 流通市值
    live = fetch_live_mv()
    if live:
        mv = {k: live[k] for k in ("创业板", "科创板", "沪深两市")}
        mv_date = live.get("date", date_str)
        mv_source = "交易所官方接口(实时)"
    else:
        mv = dict(REFERENCE_MV)
        mv_date = date_str
        mv_source = "官方参考值(静态, 接口失败回退)"

    result = {
        "date": date_str,
        "mv_date": mv_date,
        "mv_source": mv_source,
        "threshold": threshold,
        "crowding_threshold": crowding_threshold,
        "timestamp": datetime.now().isoformat(),
        "items": [],
    }

    def add(name, margin_yuan, mv_yi, crowding_ratio=None):
        if margin_yuan is None:
            result["items"].append({"name": name, "margin_yi": None, "circ_mv_yi": None,
                                    "ratio_pct": None, "is_risk": None})
            return
        m_yi = margin_yuan / 1e8
        if mv_yi and mv_yi > 0:
            r = margin_yuan / (mv_yi * 1e8) * 100
            item = {"name": name, "margin_yi": round(m_yi, 2), "circ_mv_yi": round(mv_yi, 2),
                    "ratio_pct": round(r, 2), "is_risk": r > threshold}
        else:
            item = {"name": name, "margin_yi": round(m_yi, 2), "circ_mv_yi": None,
                    "ratio_pct": None, "is_risk": None}
        if crowding_ratio is not None:
            item["crowding_ratio"] = crowding_ratio
            item["crowding_risk"] = crowding_ratio >= crowding_threshold
        result["items"].append(item)

    # 交易拥挤度
    crowding = fetch_crowding(conn, date_str) if not symbols else None
    netbuy = fetch_netbuy_trend(conn) if not symbols else None

    if symbols:
        # 个股模式（margin 表 code 为裸代码，market+code 组合消歧）
        circ = fetch_circulating_mv(conn, symbols)
        ts = f"{date_str[:4]}-{date_str[4:6]}-{date_str[6:8]} 00:00:00"
        for code in symbols:
            prefixed = code[:2] in ("sh", "sz", "bj")
            market = code[:2] if prefixed else ("sh" if code.startswith(("6", "9")) else "sz")
            bare = code[2:] if prefixed else code
            row = query_one(
                conn,
                f"SELECT margin_balance FROM margin WHERE ts='{ts}' AND market='{market}' AND code='{bare}'"
            )
            m = float(row["margin_balance"]) if row else None
            add(code, m, circ.get(code))
    else:
        for seg in DEFAULT_SEGMENTS:
            cr = crowding.get(seg, {}).get("ratio") if crowding else None
            if seg == "沪深两市":
                add("沪深两市", total_margin, mv.get("沪深两市"), crowding_ratio=cr)
            else:
                add(seg, board_margin.get(seg, 0.0), mv.get(seg), crowding_ratio=cr)

    result["crowding"] = crowding
    result["netbuy"] = netbuy
    return result


def dwidth(s):
    """显示宽度：中文/全角/emoji 算 2 列。"""
    return sum(2 if unicodedata.east_asian_width(c) in "WF" else 1 for c in s)


def fmt_row(cells, widths, aligns):
    """按显示宽度对齐的一行表格。"""
    parts = []
    for c, w, a in zip(cells, widths, aligns):
        gap = max(0, w - dwidth(c))
        parts.append(c + " " * gap if a == "<" else " " * gap + c)
    return " ".join(parts)


def format_report(result):
    """格式化文本报告。"""
    th = result["threshold"]
    cr_th = result.get("crowding_threshold", CROWDING_THRESHOLD)
    lines = [
        "=" * 70,
        "市场杠杆风险监测报告",
        f"数据日期: {result['date']}  (流通市值: {result['mv_date']}, {result.get('mv_source', '')})",
        f"风险阈值: 融资余额/流通市值 > {th}%   交易拥挤度 >= {cr_th}%",
        "=" * 70,
        "",
    ]
    headers = ("标的", "融资余额(亿)", "流通市值(亿)", "占比", "拥挤度", "状态")
    aligns = ("<", ">", ">", ">", ">", "<")
    body = []
    risk_names = []
    for it in result["items"]:
        name = it["name"]
        m_str = f"{it['margin_yi']:,.2f}" if it.get("margin_yi") is not None else "N/A"
        c_str = f"{it['circ_mv_yi']:,.2f}" if it.get("circ_mv_yi") else "N/A"
        r_str = f"{it['ratio_pct']:.2f}%" if it.get("ratio_pct") is not None else "N/A"
        cr_ratio = it.get("crowding_ratio")
        if cr_ratio is not None:
            cr_flag = "🔴" if it.get("crowding_risk") else "🟢"
            cr_str = f"{cr_ratio:.1f}%{cr_flag}"
        else:
            cr_str = "—"
        is_risk = it.get("is_risk")
        if is_risk is True:
            status = "🔴 风险"
            risk_names.append(name)
        elif is_risk is False:
            status = "🟢 安全"
        else:
            status = "❓"
        body.append((name, m_str, c_str, r_str, cr_str, status))
        if it.get("crowding_risk"):
            risk_names.append(f"{name}拥挤度{cr_ratio:.1f}%")

    widths = [max(dwidth(x) for x in col) for col in zip(headers, *body)]
    lines.append(fmt_row(headers, widths, aligns))
    lines.append("-" * (sum(widths) + len(widths) - 1))
    for row in body:
        lines.append(fmt_row(row, widths, aligns))
    lines.append("-" * (sum(widths) + len(widths) - 1))

    # 拥挤度明细
    cr = (result.get("crowding") or {}).get("沪深两市")
    if cr:
        lines.append(f"\n📊 拥挤度明细 — 全市场: 前 {cr['top_n']} 股成交 {cr['top_amount_yi']:,.2f}亿"
                     f" / 全市场 {cr['total_amount_yi']:,.2f}亿 (共 {cr['total_n']} 股)")
    else:
        lines.append("\n交易拥挤度: 未计算")

    if risk_names:
        lines.append(f"\n⚠️  风险警示: {', '.join(risk_names)}")
    else:
        lines.append(f"\n✅ 所有标的均在 {th}% 以下，杠杆风险可控。")

    # 净买入趋势
    nb = result.get("netbuy")
    if nb:
        icon = {"偏顶": "🔴", "偏底": "🟢", "中性": "⚪"}[nb["signal"]]
        lines.append(f"\n📉 融资净买入趋势 (断顶底信号, 沪深合计, 截至 {nb['date']})")
        lines.append(f"   蓝线 净买入MA{nb['ma_window']}(短期情绪): {nb['netbuy_ma']} 亿"
                     f"  近{NETBUY_LOOKBACK}日低谷 {nb['ma_trough']} 亿")
        lines.append(f"   黑线 {nb['year']}年初至今累计净买入: {nb['cum_ytd']} 亿"
                     f"  年内峰值 {nb['cum_max']} 亿, 当前 {nb['pos_at_max']}%")
        lines.append(f"   信号: {icon} {nb['signal']}")

    lines.append("\n📋 融资余额: TDengine margin 表 (fetch-margin.py 入库)")
    return "\n".join(lines)


def main():
    ap = argparse.ArgumentParser(description="市场杠杆风险监测（读 TDengine，零外部依赖）")
    ap.add_argument("--date", help="融资余额日期 YYYYMMDD (默认: 上一交易日)")
    ap.add_argument("--symbols", help="指定标的, 逗号分隔 (默认: 沪深两市+科创板+创业板)")
    ap.add_argument("--threshold", type=float, default=RISK_THRESHOLD, help=f"杠杆风险阈值%% (默认 {RISK_THRESHOLD})")
    ap.add_argument("--crowding-threshold", type=float, default=CROWDING_THRESHOLD, help=f"拥挤度阈值%% (默认 {CROWDING_THRESHOLD})")
    ap.add_argument("--json", action="store_true", help="JSON 输出")
    args = ap.parse_args()

    symbols = [s.strip() for s in args.symbols.split(",") if s.strip()] if args.symbols else None

    conn = get_conn()
    try:
        result = analyze(conn, args.date, args.threshold, symbols=symbols,
                         crowding_threshold=args.crowding_threshold)
    except (RuntimeError, ValueError) as e:
        print(f"❌ {e}", file=sys.stderr)
        sys.exit(1)

    if args.json:
        print(json.dumps(result, ensure_ascii=False, indent=2))
    else:
        print(format_report(result))


if __name__ == "__main__":
    main()
