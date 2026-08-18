#!/usr/bin/env python3
"""
某队 ETF 动向追踪 — TDengine 原生查询版。
直接查询 TDengine f10_text/finance/f10_cat 表，识别汇金/证金/社保持仓。

数据源: TDengine (tdx.f10_cat / tdx.f10_text / tdx.finance)
回退: tdx fetch-quotes --with_f10 --with_finance 在线拉取入库

用法:
  python3 find_jerry.py                        # 扫描全部重点 ETF
  python3 find_jerry.py --code sz159915        # 单只 ETF
  python3 find_jerry.py --json                 # JSON 输出
"""

import argparse
import json
import os
import re
import subprocess
import sys
from collections import defaultdict
from datetime import datetime

import taosws

# ---- 配置 ----
REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TDX_BIN = os.environ.get("TDX_BIN", os.path.join(REPO, "build", "bin", "tdx"))
TDENGINE_URL = os.environ.get("TDENGINE_URL", "taosws://root:taosdata@localhost:6041/tdx")
DB = os.environ.get("TDENGINE_DB", "tdx")

# ponytail: 不用"证金"(会误匹配"保证金")、"社保"(会误匹配基金经理履历)
# 证金全称=中国证券金融，社保=社保基金/全国社保/社保组合
GJD_KEYWORDS = [
    "汇金",
    "中国证券金融",
    "证金公司",
    "社保基金",
    "全国社保",
    "社保组合",
]

DEFAULT_ETFS = [
    ("sh510300", "华泰柏瑞沪深300ETF"),
    ("sh510050", "华夏上证50ETF"),
    ("sh510500", "南方中证500ETF"),
    ("sz159915", "易方达创业板ETF"),
    ("sh588000", "华夏科创50ETF"),
    ("sz159845", "华夏中证1000ETF"),
]


# ---- TDengine 操作 ----
def get_conn():
    return taosws.connect(TDENGINE_URL)


def query_f10_categories(conn, code: str) -> list[dict]:
    """查询某代码的 F10 目录。"""
    r = conn.query(
        f"SELECT cat_name, filename, start_pos, len_val FROM tdx.f10_cat WHERE code='{code}'"
    )
    return [{"name": row[0], "filename": row[1], "start": row[2], "length": row[3]} for row in r]


def query_f10_text(conn, code: str, cat_name: str) -> str:
    """查询某代码某章节的 F10 全文（拼接分片）。"""
    r = conn.query(
        f"SELECT content FROM tdx.f10_text "
        f"WHERE code='{code}' AND cat_name='{cat_name}' "
        f"ORDER BY seq"
    )
    parts = []
    for row in r:
        if row[0]:
            parts.append(row[0])
    return "".join(parts)


def query_finance(conn, code: str) -> dict | None:
    """查询财务数据。"""
    r = conn.query(f"SELECT * FROM tdx.finance WHERE code='{code}'")
    fields = [f.name for f in r.fields]
    for row in r:
        return dict(zip(fields, row))
    return None


def f10_data_exists(conn, code: str) -> bool:
    """检查是否已有该代码的 F10 文字数据。"""
    r = conn.query(f"SELECT count(*) FROM tdx.f10_text WHERE code='{code}'")
    for row in r:
        return row[0] > 0
    return False


# ---- 数据拉取回退 ----
def pull_data_to_tdengine(codes: list[str]):
    """调用 tdx fetch-quotes 拉取 F10+finance 数据入库。"""
    code_list = ",".join(codes)
    cmd = [TDX_BIN, "fetch-quotes", f"--quote_codes={code_list}",
           "--with_f10", "--with_finance"]
    print(f"📡 拉取数据: {' '.join(cmd)}", file=sys.stderr)
    result = subprocess.run(cmd, capture_output=True, text=True, timeout=300)
    for line in result.stderr.split("\n"):
        if line.strip():
            print(f"   {line.strip()}", file=sys.stderr)
    if result.returncode != 0:
        raise RuntimeError(f"tdx 拉取失败 (exit={result.returncode}): {result.stderr}")


def ensure_data(conn, codes: list[str]):
    """确保指定代码的 F10 数据在 TDengine 中（缺失则拉取）。"""
    missing = []
    for code in codes:
        short = code[2:] if code[:2] in ("sh", "sz", "bj") else code
        if not f10_data_exists(conn, short):
            missing.append(code)
    if missing:
        print(f"📡 {len(missing)} 只 ETF 缺 F10 数据，开始在线拉取...", file=sys.stderr)
        pull_data_to_tdengine(missing)
        # 重新连接（tdx 可能改过库状态）
        conn.close()
        return taosws.connect(TDENGINE_URL)
    return conn


# ---- F10 解析 ----
def parse_fund_shares_quarterly(text: str) -> list[dict]:
    """解析「期末基金份额」季度表。"""
    rows = []
    # 找单季度表
    idx = text.find("│指标名称/单季度")
    if idx < 0:
        return rows

    # 提取表头期间标签
    m = re.search(r'│指标名称/单季度\s+│([^│]+(?:│[^│]+)*)│', text[idx:])
    if not m:
        return rows
    parts = re.findall(r'│\s*([^│]*?)\s*(?=│)', m.group(0))
    periods = [p.strip() for p in parts if p.strip()
               and '指标名称' not in p and '单季度' not in p]

    # 提取"期末基金份额(万份)"那一整行
    section = text[idx:idx + 2000]
    share_m = re.search(r'│期末基金份额\(万份\)\s*│(.+)│\s*$', section, re.MULTILINE)
    if not share_m:
        return rows
    vals = re.findall(r'│\s*([\d,]+\.?\d*|───)\s*', '│' + share_m.group(1) + '│')

    for i, v in enumerate(vals):
        if v in ("───", "---"):
            continue
        period = periods[i] if i < len(periods) else f"col{i}"
        rows.append({"period": period, "shares_万份": float(v.replace(",", ""))})
    return rows


def parse_fund_shares_report(text: str) -> list[dict]:
    """解析报告期（年报/中报）份额表，回溯更长历史。"""
    rows = []
    idx = text.find("│指标名称/报告期")
    if idx < 0:
        return rows
    m = re.search(r'│指标名称/报告期\s+│([^│]+(?:│[^│]+)*)│', text[idx:])
    if not m:
        return rows
    parts = re.findall(r'│\s*([^│]*?)\s*(?=│)', m.group(0))
    periods = [p.strip() for p in parts if p.strip()
               and '指标名称' not in p and '报告期' not in p]

    section = text[idx:idx + 2500]
    share_m = re.search(r'│期末基金份额\(万份\)\s*│(.+)│\s*$', section, re.MULTILINE)
    if not share_m:
        return rows
    vals = re.findall(r'│\s*([\d,]+\.?\d*|───)\s*', '│' + share_m.group(1) + '│')

    for i, v in enumerate(vals):
        if v in ("───", "---"):
            continue
        period = periods[i] if i < len(periods) else f"col{i}"
        rows.append({"period": period, "shares_万份": float(v.replace(",", ""))})
    return rows


def parse_daily_shares(text: str) -> list[dict]:
    """解析「场内份额变动」日数据。"""
    rows = []
    for m in re.finditer(
        r'│\s*(\d{4}-\d{2}-\d{2})\s*│\s*([\d,]+\.?\d*)\s*│\s*(-?[\d,]+\.?\d*)\s*│',
        text
    ):
        rows.append({
            "date": m.group(1),
            "shares_万份": float(m.group(2).replace(",", "")),
            "change_万份": float(m.group(3).replace(",", "")),
        })
    return rows


def parse_holder_structure(text: str) -> list[dict]:
    """解析「持有人户数及结构」。"""
    rows = []
    # 找实际表格（第二次出现）
    idx = text.find("持有人户数及结构】")
    if idx < 0:
        return rows
    idx2 = text.find("持有人户数及结构】", idx + 1)
    if idx2 >= 0:
        idx = idx2

    section = text[idx:idx + 1500]
    # 表头
    m = re.search(r'│报告期\s+│\s*([^│]+(?:│[^│]+)*)│', section)
    if not m:
        return rows
    parts = re.findall(r'│\s*([^│]*?)\s*(?=│)', m.group(0))
    dates = [p.strip() for p in parts if p.strip() and '报告期' not in p]

    # 提取各行
    def _vals(label):
        idx_l = section.find(label)
        if idx_l < 0:
            return []
        line = section[idx_l:section.find("\n", idx_l)]
        return [v for v in re.findall(r'│\s*([\d,]+\.?\d*|───)\s*', line) if v not in ("───", "---")]

    inst_r = _vals("机构持有份额比例")
    indi_r = _vals("个人持有份额比例")
    inst_s = _vals("机构持有份额(万份)")
    indi_s = _vals("个人持有份额(万份)")

    for i, d in enumerate(dates):
        row = {"report_date": d}
        if i < len(inst_r):
            row["inst_ratio_pct"] = float(inst_r[i].replace(",", ""))
        if i < len(indi_r):
            row["indi_ratio_pct"] = float(indi_r[i].replace(",", ""))
        if i < len(inst_s):
            row["inst_shares_万份"] = float(inst_s[i].replace(",", ""))
        if i < len(indi_s):
            row["indi_shares_万份"] = float(indi_s[i].replace(",", ""))
        rows.append(row)
    return rows


def parse_top10_holders(text: str) -> list[dict]:
    """解析「基金十大持有人」。"""
    holders = []
    blocks = re.split(r'截止日期[:：]\s*(\d{4}-\d{2}-\d{2})', text)
    for i in range(1, len(blocks), 2):
        date = blocks[i]
        block = blocks[i + 1] if i + 1 < len(blocks) else ""
        for m in re.finditer(
            r'│\s*(.+?)\s*│\s*([\d,]+\.?\d*)\s*│\s*(\d+\.?\d*)\s*│',
            block
        ):
            name = m.group(1).strip().lstrip("│").strip()
            if "持有人名称" in name or "持有份额" in name or not name:
                continue
            holders.append({
                "date": date,
                "name": name,
                "shares_万份": float(m.group(2).replace(",", "")),
                "ratio_pct": float(m.group(3)),
                "is_gjd": any(kw in name for kw in GJD_KEYWORDS),
            })
    return holders


def parse_etf_holdings(text: str) -> list[dict]:
    """解析「持股情况」章节，提取每季度 ETF 底层持仓明细。"""
    holdings = []
    blocks = re.split(r'截止日期[:：]\s*(\d{4}-\d{2}-\d{2})', text)
    for i in range(1, len(blocks), 2):
        date = blocks[i]
        block = blocks[i + 1] if i + 1 < len(blocks) else ""
        # 匹配: │300750│宁德时代│2501.44│1004828.17│0.587629│19.69│
        for m in re.finditer(
            r'│\s*(\d{6})\s*│\s*(.+?)\s*│\s*([\d,]+\.?\d*)\s*│\s*([\d,]+\.?\d*)\s*│\s*([\d,]+\.?\d*)\s*│\s*(\d+\.?\d*)\s*│',
            block
        ):
            holdings.append({
                "date": date,
                "stock_code": m.group(1),
                "stock_name": m.group(2).strip(),
                "shares_万股": float(m.group(3).replace(",", "")),
                "market_value_万元": float(m.group(4).replace(",", "")),
                "flow_ratio_pct": float(m.group(5)),
                "nav_ratio_pct": float(m.group(6)),
            })
    return holdings


def compare_holdings(holdings: list[dict]) -> dict:
    """
    对比相邻两个季度的持仓变化：新增/退出/增持/减持。
    返回变动摘要。
    """
    if len(holdings) < 2:
        return {}
    by_date = defaultdict(list)
    for h in holdings:
        by_date[h["date"]].append(h)

    dates = sorted(by_date.keys())
    if len(dates) < 2:
        return {}
    d1, d2 = dates[-2], dates[-1]  # 最近两个季度
    h1 = {h["stock_code"]: h for h in by_date[d1]}
    h2 = {h["stock_code"]: h for h in by_date[d2]}

    codes_all = set(h1.keys()) | set(h2.keys())
    added = [c for c in codes_all if c not in h1]   # 新增
    removed = [c for c in codes_all if c not in h2] # 退出
    increased = []   # 增持
    decreased = []   # 减持
    for c in codes_all - set(added) - set(removed):
        delta = h2[c]["nav_ratio_pct"] - h1[c]["nav_ratio_pct"]
        if delta > 0.3:
            increased.append((c, h2[c]["stock_name"], delta))
        elif delta < -0.3:
            decreased.append((c, h2[c]["stock_name"], abs(delta)))

    return {
        "from": d1,
        "to": d2,
        "added": [(c, h2[c]["stock_name"], h2[c]["nav_ratio_pct"]) for c in added],
        "removed": [(c, h1[c]["stock_name"], h1[c]["nav_ratio_pct"]) for c in removed],
        "increased": sorted(increased, key=lambda x: -x[2]),
        "decreased": sorted(decreased, key=lambda x: -x[2]),
        "top5_now": sorted(by_date[d2], key=lambda x: -x["nav_ratio_pct"])[:5],
    }


# ---- 主分析 ----
def analyze_etf(conn, code_raw: str, name: str) -> dict:
    """分析单只 ETF 的某队持仓。"""
    _p = lambda *a, **kw: print(*a, file=sys.stderr, **kw)
    code = code_raw[2:] if code_raw[:2] in ("sh", "sz", "bj") else code_raw

    _p(f"\n{'='*60}")
    _p(f"📊 {name} ({code_raw})")
    _p(f"{'='*60}")

    text = query_f10_text(conn, code, "基金份额")
    if not text:
        _p("  ⚠️ 无「基金份额」F10 数据")
        return {"code": code_raw, "name": name, "error": "no_f10_text"}

    result = {"code": code_raw, "name": name, "fetch_time": datetime.now().isoformat()}

    # 1. 季度份额 + 年报/中报
    quarterly = parse_fund_shares_quarterly(text)
    report_period = parse_fund_shares_report(text)
    if quarterly:
        _p(f"\n  📈 基金份额（季度）:")
        for r in quarterly:
            _p(f"     {r['period']}: {r['shares_万份']/10000:.2f}亿份")
    if report_period:
        _p(f"\n  📋 基金份额（报告期）:")
        for r in report_period:
            _p(f"     {r['period']}: {r['shares_万份']/10000:.2f}亿份")

    # 2. 日份额变动
    daily = parse_daily_shares(text)
    if daily:
        latest = daily[0]
        delta = latest["change_万份"]
        arrow = "↑" if delta > 0 else "↓" if delta < 0 else "→"
        _p(f"\n  📊 场内份额（日）最新:")
        _p(f"     {latest['date']}: {latest['shares_万份']/10000:.2f}亿份 ({arrow}{abs(delta)/10000:.2f}亿)")

    # 3. 持有人结构
    struct = parse_holder_structure(text)
    if struct:
        s = struct[0]
        _p(f"\n  👥 持有人结构（{s['report_date']}）:")
        if isinstance(s.get('inst_ratio_pct'), float):
            _p(f"     机构: {s['inst_ratio_pct']:.1f}%  个人: {s['indi_ratio_pct']:.1f}%")

    # 4. 十大持有人
    top10 = parse_top10_holders(text)
    if top10:
        by_date = defaultdict(list)
        for h in top10:
            by_date[h["date"]].append(h)

        for date in sorted(by_date.keys(), reverse=True):
            holders = by_date[date]
            gjd = [h for h in holders if h["is_gjd"]]
            others = [h for h in holders if not h["is_gjd"]]

            if gjd:
                _p(f"\n  🏛️ 十大持有人（{date}）- 某队:")
                g_total = g_ratio = 0
                for h in gjd:
                    _p(f"     {h['name']}: {h['shares_万份']/10000:.2f}亿份 ({h['ratio_pct']:.1f}%)")
                    g_total += h["shares_万份"]
                    g_ratio += h["ratio_pct"]
                _p(f"     ─────────────────")
                _p(f"     🔴 某队合计: {g_total/10000:.2f}亿份 ({g_ratio:.1f}%)")

            if others:
                _p(f"\n  🏢 其他主要持有人（{date}）:")
                for h in others[:5]:
                    _p(f"     {h['name']}: {h['shares_万份']/10000:.2f}亿份 ({h['ratio_pct']:.1f}%)")

    result.update({
        "quarterly_shares": quarterly,
        "report_shares": report_period,
        "daily_shares": daily[:5] if daily else [],
        "holder_structure": struct[:1] if struct else [],
        "top10_holders": top10,
    })

    # 4.5 持股明细对比（季度底层持仓变动）
    holdings_text = query_f10_text(conn, code, "持股情况")
    if holdings_text:
        holdings = parse_etf_holdings(holdings_text)
        if holdings:
            cmp = compare_holdings(holdings)
            result["holdings_cmp"] = cmp
            if cmp.get("top5_now"):
                _p(f"\n  🎯 最新底层持仓 TOP5（{cmp['to']}）:")
                for h in cmp["top5_now"]:
                    _p(f"     {h['stock_code']} {h['stock_name']:8s} {h['nav_ratio_pct']:5.1f}% ({h['market_value_万元']/10000:.1f}亿)")
            if cmp.get("added"):
                _p(f"\n  🆕 新进入 ({len(cmp['added'])} 只):")
                for c, n, r in cmp["added"][:5]:
                    _p(f"     +{c} {n} ({r:.1f}%)")
            if cmp.get("removed"):
                _p(f"\n  🚫 已退出 ({len(cmp['removed'])} 只):")
                for c, n, r in cmp["removed"][:5]:
                    _p(f"     -{c} {n} (原{r:.1f}%)")
            if cmp.get("increased"):
                _p(f"\n  📈 增持 ({len(cmp['increased'])} 只):")
                for c, n, d in cmp["increased"][:5]:
                    _p(f"     ↑{c} {n} (+{d:.1f}%)")
            if cmp.get("decreased"):
                _p(f"\n  📉 减持 ({len(cmp['decreased'])} 只):")
                for c, n, d in cmp["decreased"][:5]:
                    _p(f"     ↓{c} {n} (-{d:.1f}%)")

    # 5. 综合判断
    if daily and quarterly:
        _p(f"\n  {'='*50}")
        _p(f"  💡 综合判断:")
        trend_5d = sum(r["change_万份"] for r in daily[:5])
        if trend_5d < -10000:
            _p(f"     ⚠️ 近5日份额持续减少 ({abs(trend_5d)/10000:.1f}亿)，大资金可能在减仓")
        elif trend_5d > 10000:
            _p(f"     ✅ 近5日份额增加 ({trend_5d/10000:.1f}亿)，大资金可能在加仓")

        if len(quarterly) >= 2:
            q_now = quarterly[0]["shares_万份"]
            q_prev = quarterly[1]["shares_万份"]
            if q_prev > 0:
                delta_pct = (q_now - q_prev) / q_prev * 100
                if delta_pct < -10:
                    _p(f"     🔴 季度降幅 {abs(delta_pct):.0f}%（{quarterly[0]['period']} vs {quarterly[1]['period']}），主力大幅减仓！")
                elif delta_pct < -5:
                    _p(f"     🟡 季度降幅 {abs(delta_pct):.0f}%（{quarterly[0]['period']} vs {quarterly[1]['period']}），主力在减仓")
                else:
                    _p(f"     🟢 季度变化: {delta_pct:+.0f}%")

        # 对比 2024年报 vs 2025年报（若报告期数据可用）
        if report_period and len(report_period) >= 2:
            rp_2025 = next((r for r in report_period if '2025年报' in r['period']), None)
            rp_2024 = next((r for r in report_period if '2024年报' in r['period']), None)
            if rp_2025 and rp_2024 and rp_2024["shares_万份"] > 0:
                delta_rp = (rp_2025["shares_万份"] - rp_2024["shares_万份"]) / rp_2024["shares_万份"] * 100
                if delta_rp < -10:
                    _p(f"     🔴 年报降幅 {abs(delta_rp):.0f}%（2025年报 vs 2024年报），牛市底部筹码已大量转移！")
                elif delta_rp < -5:
                    _p(f"     🟡 年报降幅 {abs(delta_rp):.0f}%（2025年报 vs 2024年报）")
                else:
                    _p(f"     🟢 年报变化: {delta_rp:+.0f}%")

    return result


def main():
    parser = argparse.ArgumentParser(description="某队 ETF 动向追踪 (TDengine)")
    parser.add_argument("--code", help="指定 ETF 代码，如 sz159915")
    parser.add_argument("--json", action="store_true")
    parser.add_argument("--pull", action="store_true", help="强制重新拉取数据")
    args = parser.parse_args()

    etfs = [(args.code, args.code)] if args.code else DEFAULT_ETFS

    conn = get_conn()

    # 确保数据就绪
    if args.pull:
        codes = [c for c, _ in etfs]
        print(f"📡 强制拉取 {len(codes)} 只 ETF...", file=sys.stderr)
        pull_data_to_tdengine(codes)
        conn.close()
        conn = get_conn()
    else:
        codes = [c for c, _ in etfs]
        conn = ensure_data(conn, codes)

    all_results = []
    for code, name in etfs:
        r = analyze_etf(conn, code, name)
        all_results.append(r)

    if args.json:
        cleaned = [{k: v for k, v in r.items()} for r in all_results]
        print(json.dumps(cleaned, ensure_ascii=False, indent=2))

    conn.close()


if __name__ == "__main__":
    main()
