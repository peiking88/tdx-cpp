"""选股脚本共享工具 (scalper/leader/smmd)。

提取自三脚本逐字重复的代码: 代码解析、自选股读取、全市场标的池。
"""

import os

# 自选股板块文件 (通达信 zxg.blk)。环境变量 TDX_ZXG_BLK 可覆盖。
ZXG_PATH = os.environ.get("TDX_ZXG_BLK", "/home/li/.local/share/tdxcfv/drive_c/tc/T0002/blocknew/zxg.blk")


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
    """全量 A 股标的 (SH/SZ/BJ 三市场, 排除基金/指数/B股/债券/回购)。"""
    try:
        r = conn.query(
            "SELECT code, market FROM tdx.stock_name "
            "WHERE market IN ('sh','sz','bj') AND ("
            "  code LIKE '60%' OR code LIKE '68%'"      # SH 主板 / 科创板
            "  OR code LIKE '00%' OR code LIKE '30%'"   # SZ 主板 / 创业板
            "  OR code LIKE '43%' OR code LIKE '83%' OR code LIKE '87%' OR code LIKE '920%'"  # BJ 北交所
            ")"
        )
        return [(m, c) for c, m in r]
    except Exception:
        return []
