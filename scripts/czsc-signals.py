#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
czsc-signals：启动盘中分析 cli，循环读 TDengine signals 表，
三类买卖点 + 多空方向 表格展示。（终端仅展示表格，其余写日志）

表头：代码、名称、最新价、涨跌幅、周、日、30m、5m
每格两行：
  上行 = 方向（多/空，青色=多/黄色=空）
  下行 = 买/卖信号（红=做多类/绿=卖空类，粗体=信号变化）

用法：
  python3 scripts/czsc-signals.py            # 默认 zxg，cli 5分钟一轮，看板 10s 刷新
  python3 scripts/czsc-signals.py --no-cli   # 仅看板（读已有 signals）
  python3 scripts/czsc-signals.py --codes sh510300,sz002741

依赖：taosws（已装）
"""
import argparse
import logging
import os
import re
import subprocess
import sys
import time
import unicodedata
from datetime import datetime

import taosws

# ---- 配置 ----
WS_URL = os.getenv("CZSC_WS_URL", "taosws://root:taosdata@localhost:6041/tdx")
REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CLI_BIN = os.getenv("CZSC_CLI", os.path.join(REPO_ROOT, "build", "bin", "czsc_cli"))
LOG_DIR = os.getenv("CZSC_LOG_DIR", os.path.join(REPO_ROOT, "output"))

FREQ_COL = {"week": "周", "D": "日", "F30": "30m", "F5": "5m"}
COL_ORDER = ["week", "D", "F30", "F5"]

# k3 含这些子串 → 买卖点信号（cxt_first_buy/sell、cxt_second_bs、cxt_third_bs/buy、tas_macd_bs1 等）
BS_KEYWORDS = ("Buy", "Sell", "Bs")

# ---- 信号分类 ----
# 一/二/三买卖点（最精确，优先展示）
BS_POINTS = {"一买", "一卖", "二买", "二卖", "三买", "三卖"}
# 买卖信号
BS_TRADE = {"买", "卖", "买点", "卖点"}
# 多空方向（含 TasDoubleMa 的 多头/空头）
BS_DIR = {"多", "空", "多头", "空头"}

# 买/卖优先级（取最显著的一个）
BS_PRIO = {
    "一买": 6, "一卖": 6, "二买": 6, "二卖": 6, "三买": 6, "三卖": 6,
    "买": 5, "卖": 5, "买点": 5, "卖点": 5,
}
DIR_PRIO = {"多": 1, "空": 1, "多头": 1, "空头": 1}

# 信号族（用于同优先级判定）
K3_PRIO = {"cxt_": 3, "tas_": 2, "zdy_": 1, "jcc_": 0}


def classify_v1(v1):
    """将 v1 字面量分类: 'point'(一/二/三买卖点), 'trade'(买/卖), 'dir'(多/空/多头/空头), None"""
    if v1 in BS_POINTS:
        return "point"
    if v1 in BS_TRADE:
        return "trade"
    if v1 in BS_DIR:
        return "dir"
    return None


def normalize_dir(v1):
    """方向标准化：多头→多，空头→空"""
    if v1 == "多头":
        return "多"
    if v1 == "空头":
        return "空"
    return v1


# 买卖点集合（用于矛盾检测）
_BUY_SIGNALS = {"买", "买点", "一买", "二买", "三买", "开多"}
_SELL_SIGNALS = {"卖", "卖点", "一卖", "二卖", "三卖", "开空"}


def _is_contradiction(d, b):
    """方向与买卖点矛盾：多+卖空类 / 空+做多类"""
    if d == "多" and b in _SELL_SIGNALS:
        return True
    if d == "空" and b in _BUY_SIGNALS:
        return True
    return False


def setup_logger():
    os.makedirs(LOG_DIR, exist_ok=True)
    log_path = os.path.join(LOG_DIR, "czsc-signals.log")
    lg = logging.getLogger("czsc-signals")
    lg.setLevel(logging.INFO)
    lg.handlers.clear()
    fh = logging.FileHandler(log_path, encoding="utf-8")
    fh.setFormatter(logging.Formatter("%(asctime)s %(levelname)s %(message)s",
                                      datefmt="%H:%M:%S"))
    lg.addHandler(fh)
    return lg, log_path


# ---- TDengine 访问 ----
def connect(url=WS_URL):
    return taosws.connect(url)


def query(cur, sql, params=()):
    cur.execute(sql, params)
    return cur.fetchall()


def load_name_map(cur):
    rows = query(cur, "SELECT code, market, name FROM stock_name")
    return {(c, m): n for c, m, n in rows}


def _median(vals):
    s = sorted(vals)
    n = len(s)
    return s[n // 2] if n % 2 else (s[n // 2 - 1] + s[n // 2]) / 2


def load_quantities(cur, codes):
    out = {}
    for (mkt, code) in codes:
        tb = f"k_{mkt}{code}_1d"
        try:
            rows = query(cur, f"SELECT close FROM {tb} ORDER BY ts DESC LIMIT 5")
        except Exception:
            rows = []
        closes = [r[0] for r in rows]
        if not closes:
            continue
        if len(closes) >= 3:
            med = _median(closes)
            good = [c for c in closes if med and abs(c - med) / med <= 0.5]
            if len(good) >= 2:
                price, prev = good[0], good[1]
                out[code] = {"price": price, "chg": (price - prev) / prev * 100}
                continue
        last = closes[0]
        prev = closes[1] if len(closes) > 1 else last
        out[code] = {"price": last, "chg": (last - prev) / prev * 100 if prev else 0.0}
    return out


def load_cxt_signals(cur):
    """加载所有买卖点信号，按 (code,market,freq) 分类为 方向/买卖/类买卖点。

    兼容新旧两种 k3 命名：
      旧（stub）：CxtFirstBuyV221126、CxtBsV240526（含 Cxt 前缀）
      新（实现）：BUY1、SELL1、BS2辅助、BS3辅助、第二买卖点、三买辅助

    返回：
      dir_sig:  {(code,mkt,freq): v1}  多/空方向
      bs_sig:   {(code,mkt,freq): v1}  买/卖/一/二/三买卖点（最显著的一个）
    """
    rows = query(
        cur,
        "SELECT code, market, freq, ts, sig FROM signals",
    )
    # 按 (code,mkt,freq) 收集所有信号
    by_key = {}  # key -> list of (v1, category, prio)
    for code, mkt, freq, ts, sig in rows:
        parts = sig.split("_")
        if len(parts) < 7:
            continue
        k3, v1, score_s = parts[2], parts[3], parts[-1]
        # 兼容新旧格式：
        #   含 Buy/Sell/Bs 前缀（cxt_first_buy/sell、tas_macd_bs1 等）
        #   新格式 BUY1/SELL1/BS2辅助/BS3辅助/第二买卖点/三买辅助
        #   多空方向：TasFirstBs/TasDoubleMa/TasHlma（k3 含 DoubleMa/Hlma/BS1）
        if not (any(kw in k3 for kw in BS_KEYWORDS) or k3 in
                ("BUY1", "SELL1") or "BS2" in k3 or "BS3" in k3
                or "第二买卖点" in k3 or "三买辅助" in k3
                or "DoubleMa" in k3 or "Hlma" in k3 or k3.startswith("BS1")):
            continue
        cat = classify_v1(v1)
        if cat is None:
            continue
        try:
            score = int(score_s)
        except ValueError:
            score = 0
        prio = (BS_PRIO.get(v1, 0) if cat != "dir" else DIR_PRIO.get(v1, 0))
        k3p = K3_PRIO.get(k3[:4].lower(), 0)  # 统一小写匹配
        key = (code, mkt, freq)
        by_key.setdefault(key, []).append((v1, cat, prio, k3p, score, ts))

    dir_sig, bs_sig = {}, {}
    for key, items in by_key.items():
        # 方向类：取最高 prio，标准化 多头→多/空头→空；同分选最新 ts
        dirs = [(v, p, k, s, t) for v, c, p, k, s, t in items if c == "dir"]
        if dirs:
            dirs.sort(key=lambda x: (x[1], x[2], x[3], x[4]), reverse=True)
            dir_sig[key] = normalize_dir(dirs[0][0])
        # 买卖类（含类买卖点）：取最高 prio；同分选最新 ts
        trades = [(v, p, k, s, t) for v, c, p, k, s, t in items if c in ("point", "trade")]
        if trades:
            trades.sort(key=lambda x: (x[1], x[2], x[3], x[4]), reverse=True)
            bs_sig[key] = trades[0][0]
    return dir_sig, bs_sig


# ---- ANSI 颜色 ----
_RESET = "\033[0m"
_BOLD = "\033[1m"
_RED = "\033[31m"       # 做多类（买/一买/二买/三买）
_GREEN = "\033[32m"     # 卖空类（卖/一卖/二卖/三卖）
_CYAN = "\033[36m"      # 多（方向）
_YELLOW = "\033[33m"    # 空（方向）
_DIM = "\033[2m"
_ANSI_RE = re.compile(r"\033\[[0-9;]*m")


def _vis_width(s):
    s = _ANSI_RE.sub("", s)
    return sum(2 if unicodedata.east_asian_width(c) in "WF" else 1 for c in s)


def _color_bs(v1, changed):
    """买/卖着色：红=做多/绿=卖空，粗体=变化"""
    if not v1:
        return f"{_DIM}-{_RESET}"
    if v1 in {"买", "买点", "一买", "二买", "三买"}:
        color = _RED
    elif v1 in {"卖", "卖点", "一卖", "二卖", "三卖"}:
        color = _GREEN
    else:
        color = ""
    if color:
        return f"{_BOLD}{color}{v1}{_RESET}" if changed else f"{color}{v1}{_RESET}"
    return f"{_BOLD}{v1}{_RESET}" if changed else v1


def _color_dir(v1, changed):
    """方向着色：红=做多(多)/绿=做空(空)，与买卖点色系统一"""
    if not v1:
        return ""
    if v1 == "多":
        color = _RED
    elif v1 == "空":
        color = _GREEN
    else:
        color = ""
    if color:
        return f"{_BOLD}{color}{v1}{_RESET}" if changed else f"{color}{v1}{_RESET}"
    return f"{_BOLD}{v1}{_RESET}" if changed else v1


def _fmt_cell(d, b, changed):
    """单行紧凑格式: 多(一买) / 空(二卖) / 多(不明) / 一卖 / 不明
    矛盾组合(多+卖/空+买)追加⚠并改用黄色警示。"""
    d_show = d if d else ""
    b_show = b if b else ""
    if not d_show and not b_show:
        return f"{_DIM}-{_RESET}"
    contrad = _is_contradiction(d_show, b_show)
    if d_show and b_show:
        core = f"{d_show}({b_show})"
    elif d_show:
        core = f"{d_show}(不明)"
    else:
        core = b_show
    if contrad:
        core = f"{core}⚠"
    # 着色：矛盾=黄色警示；否则整格按方向色（多=红/空=绿）或买卖点色
    if contrad:
        color = _YELLOW
    elif d_show == "多":
        color = _RED
    elif d_show == "空":
        color = _GREEN
    elif b_show in _BUY_SIGNALS:
        color = _RED
    elif b_show in _SELL_SIGNALS:
        color = _GREEN
    else:
        color = ""
    if color:
        return f"{_BOLD}{color}{core}{_RESET}" if changed else f"{color}{core}{_RESET}"
    return f"{_BOLD}{core}{_RESET}" if changed else core


def render(name_map, qty, dir_sig, bs_sig, prev_bs, changed):
    """渲染表格到 stdout。每格单行紧凑格式: 多(一买)/空(二卖)/多(不明)。"""
    keys = set(dir_sig.keys()) | set(bs_sig.keys())
    codes_seen = sorted({(mkt, code) for (code, mkt, _freq) in keys})
    if not codes_seen:
        sys.stdout.write("\033[2J\033[H暂无买卖点信号\n")
        return

    headers = ["代码", "名称", "最新价", "涨跌幅"] + [FREQ_COL[ft] for ft in COL_ORDER]
    aligns = ["left", "left", "right", "right"] + ["center"] * 4

    raw_rows = []
    for mkt, code in codes_seen:
        disp = mkt + code
        name = name_map.get((code, mkt), "")
        q = qty.get(code, {})
        price = f"{q['price']:.2f}" if "price" in q else "-"
        chg = q.get("chg")
        if chg is None:
            chg_s = "-"
        else:
            flag = "⚠" if abs(chg) > 20 else ""
            chg_s = f"{chg:+.2f}%{flag}"
        cols = []
        for ft in COL_ORDER:
            key = (code, mkt, ft)
            d = dir_sig.get(key, "")
            b = bs_sig.get(key, "")
            cols.append(_fmt_cell(d, b, key in changed))
        raw_rows.append([disp, name, price, chg_s] + cols)

    widths = [_vis_width(h) for h in headers]
    for row in raw_rows:
        for i, cell in enumerate(row):
            widths[i] = max(widths[i], _vis_width(cell))
    widths = [max(8, w) for w in widths]

    sep = "  ".join("-" * w for w in widths)
    out = "\033[2J\033[H"
    out += "  ".join(_pad(headers[i], widths[i], aligns[i]) for i in range(len(headers))) + "\n"
    out += sep + "\n"
    out += "\n".join("  ".join(_pad(row[i], widths[i], aligns[i]) for i in range(len(headers)))
                    for row in raw_rows) + "\n"
    sys.stdout.write(out)


def _pad(s, width, align="left"):
    w = _vis_width(s)
    pad = max(0, width - w)
    if align == "right":
        return " " * pad + s
    if align == "center":
        l = pad // 2
        return " " * l + s + " " * (pad - l)
    return s + " " * pad


# ---- main ----
def main():
    p = argparse.ArgumentParser(description="缠论三类买卖点看板")
    p.add_argument("--ws", default=WS_URL, help="taosws 连接串")
    p.add_argument("--cli-interval", type=int, default=300,
                   help="czsc_cli 分析间隔秒（默认 300）")
    p.add_argument("--refresh", type=int, default=10, help="看板刷新秒（默认 10）")
    p.add_argument("--no-cli", action="store_true", help="不启动分析 cli")
    p.add_argument("--codes", help="指定标的如 sh510300,sz002741（覆盖 blk）")
    args = p.parse_args()

    log, log_path = setup_logger()
    log.info("启动 czsc-signals refresh=%ds cli_interval=%ds ws=%s",
             args.refresh, args.cli_interval, args.ws)
    log.info("信号分类: 方向(多/空) + 买/卖(一/二/三买卖点)；红色系=做多(多/买/一买/二买/三买) 绿色系=做空(空/卖/一卖/二卖/三卖)；⚠黄色=方向与买卖点矛盾")

    proc = None
    proc_log = None
    if not args.no_cli:
        cmd = [CLI_BIN, "--interval", str(args.cli_interval)]
        if args.codes:
            cmd += ["--codes", args.codes]
        if os.path.isfile(CLI_BIN):
            proc_log = open(log_path, "a", encoding="utf-8")
            proc = subprocess.Popen(cmd, stdout=proc_log, stderr=proc_log)
            log.info("启动分析 cli：%s（进度写入 %s）", " ".join(cmd), log_path)
        else:
            log.warning("未找到 %s，只读模式运行看板", CLI_BIN)

    name_map = {}
    prev_bs = {}
    running = True
    try:
        while running:
            try:
                conn = connect(args.ws)
                cur = conn.cursor()
                nm = load_name_map(cur)
                if nm:
                    name_map = nm
                dir_sig, bs_sig = load_cxt_signals(cur)
                disp_codes = sorted({(mkt, code) for code, mkt, _ in
                                     set(dir_sig.keys()) | set(bs_sig.keys())})
                qty = load_quantities(cur, disp_codes)
                conn.close()
                # 检测买/卖信号变化
                changed = {k for k, v in bs_sig.items() if prev_bs.get(k) != v}
                if changed:
                    samples = sorted(changed)[:5]
                    log.info("买/卖信号变化 %d 处：%s", len(changed),
                             ", ".join(f"{c}/{m}/{f}:{prev_bs.get(k,'-')}→{bs_sig[k]}"
                                       for k in samples for (c, m, f) in [k]))
                prev_bs = bs_sig
                render(name_map, qty, dir_sig, bs_sig, prev_bs, changed)
            except Exception as e:
                log.error("读取失败：%s，%ds 后重试", e, args.refresh)
                sys.stdout.write("\033[2J\033[H读取失败：%s，重试中...\n" % e)
            time.sleep(args.refresh)
    except KeyboardInterrupt:
        running = False
        log.info("用户中断退出")
    finally:
        if proc and proc.poll() is None:
            proc.terminate()
            try:
                proc.wait(timeout=3)
            except subprocess.TimeoutExpired:
                proc.kill()
            except KeyboardInterrupt:
                proc.kill()
            log.info("cli 子进程已终止")
        if proc_log is not None:
            proc_log.close()


if __name__ == "__main__":
    main()
