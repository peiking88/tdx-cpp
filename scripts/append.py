#!/usr/bin/env python3
# append.py：读取 stock_name → 以 stock_name 为代码源增量导入 vipdoc + 网络最新数据入库。
#
# 核心复用 tdx import 引擎（vipdoc 本地增量 + 网络补缺 + 复权因子 + 清理），
# 本脚本仅做「读 stock_name → 组装代码 → 调用 tdx import」的编排与报告，
# 不重写导入逻辑（ponytail：已有 C++ 实现经过实测，Python 重写易错且冗余）。
#
# 代码以位置参数传入 tdx import（非 blk 文件）——blk 格式仅支持 sh/sz 前缀，
# 无法表示北交所 bj 代码；位置参数走 bare_codes 解析，sh/sz/bj 三市均支持。
#
# 与 tdx import --all 的区别：
#   --all 内部先 SyncStockNames（网络重拉码表，~分钟级）再导入；
#   本脚本先读 stock_name 报告代码数，确认非空后再调，避免空码表白跑。
#
# 用法：
#   python3 scripts/append.py                    # 全量 A 股（stock_name 全市场）
#   python3 scripts/append.py --dry-run          # 仅报告代码数，不执行
#   python3 scripts/append.py --codes sh600000 sz000001
#   python3 scripts/append.py --codes-file my_codes.txt
#   python3 scripts/append.py --daily-only       # 仅日 K（跳过 1m/5m + 复权）
#   python3 scripts/append.py --jobs 8           # 8 线程本地导入
#   python3 scripts/append.py --keep-intraday    # 保留当日盘中（不清今日）
#   python3 scripts/append.py --czsc             # import 后 czsc 信号落库（盘后批处理）
#   python3 scripts/append.py --czsc --czsc-dry  # czsc 仅分析不写 DB
#
# 环境变量：TDX_BIN  TAOS  TDX_TAOS_DB  TDX_HOME  TDX_ZXG_BLK
import argparse
import os
import subprocess
import sys
import time

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TDX_BIN = os.environ.get("TDX_BIN", os.path.join(REPO, "build", "bin", "tdx"))
TAOS = os.environ.get("TAOS", "taos")
DB = os.environ.get("TDX_TAOS_DB", "tdx")


def IsHkCode(code):
    # 复刻 C++ tdx::IsHkCode（include/tdx/consts.hpp）。
    if len(code) >= 8 and code[:2] in ("sh", "sz", "bj"):
        return False
    if not code:
        return False
    all_digit = all('0' <= c <= '9' for c in code)
    if all_digit:
        return len(code) in (4, 5)
    return code[0].isalpha()


def taos_query(sql):
    r = subprocess.run([TAOS, "-s", f"USE {DB}; {sql}"], capture_output=True, text=True)
    if r.returncode != 0:
        sys.exit(f"[FAIL] taos SQL 失败: {sql}\n{r.stderr.strip()}")
    return r.stdout


def read_stock_names():
    # 读 stock_name 表，返回 [(market, code), ...]（market 为 sh/sz/bj，code 为 6 位裸码）。
    out = []
    r = subprocess.run([TAOS, "-s", f"USE {DB}; SELECT code, market FROM stock_name"],
                       capture_output=True, text=True)
    for ln in r.stdout.splitlines():
        parts = ln.split("|")
        if len(parts) >= 2:
            code = parts[0].strip()
            market = parts[1].strip()
            if code.isdigit() and len(code) == 6 and market:
                out.append((market, code))
    return out


def fmt(secs):
    if secs < 60:
        return f"{secs:.1f}s"
    m, s = divmod(int(secs), 60)
    h, m = divmod(m, 60)
    return f"{secs:.1f}s ({h}h{m}m{s}s)" if h else f"{secs:.1f}s ({m}m{s}s)"


def main():
    ap = argparse.ArgumentParser(
        description="读 stock_name → vipdoc + 网络增量导入最新数据入库")
    ap.add_argument("--codes", nargs="*", help="显式代码（带 sh/sz/bj 前缀），覆盖 stock_name")
    ap.add_argument("--codes-file", help="代码文件，每行一个（# 开头注释）")
    ap.add_argument("--dry-run", action="store_true", help="仅报告代码数与命令，不执行")
    ap.add_argument("--daily-only", action="store_true", help="仅导入日 K（跳过 1m/5m + 复权因子）")
    ap.add_argument("--keep-intraday", action="store_true", help="保留当日盘中数据（不清今日）")
    ap.add_argument("--jobs", type=int, default=0, help="本地导入线程数（0=自动=CPU 核数）")
    ap.add_argument("--with-hk", action="store_true",
                    help="保留港股代码（默认跳过）——vipdoc/网络 K 线仅 A 股，HK 会被 tdx import 过滤")
    ap.add_argument("--czsc", action="store_true",
                    help="import 后运行 tdx czsc 盘后落库缠论信号（signals 表覆盖式写入）")
    ap.add_argument("--czsc-freqs", default="F5,F30,D,week",
                    help="czsc 周期子集 (默认 F5,F30,D,week)")
    ap.add_argument("--czsc-dry", action="store_true",
                    help="czsc 仅分析不写 DB（dry-run）")
    args = ap.parse_args()

    if not os.path.exists(TDX_BIN):
        sys.exit(f"找不到 tdx 二进制: {TDX_BIN}（可用 TDX_BIN 覆盖）")

    # 1. 确定代码源
    if args.codes:
        raw_codes = args.codes
    elif args.codes_file:
        raw_codes = []
        with open(args.codes_file) as f:
            for ln in f:
                ln = ln.strip()
                if not ln or ln.startswith("#"):
                    continue
                raw_codes.append(ln.split()[0])
    else:
        stock = read_stock_names()
        if not stock:
            sys.exit(f"stock_name 表为空（{DB}.stock_name），请先 tdx fetch-names 或检查数据库。")
        # 去重（stock_name 理论唯一 (code, market)，防御性去重）
        seen = set()
        raw_codes = []
        for m, c in stock:
            key = (m, c)
            if key not in seen:
                seen.add(key)
                raw_codes.append(m + c)

    # 2. 分类 A 股 / 港股（带显式 sh/sz/bj 前缀的全部视为 A 股）
    a_codes = []
    hk_codes = []
    for raw in raw_codes:
        if len(raw) >= 8 and raw[:2] in ("sh", "sz", "bj"):
            a_codes.append(raw)
        elif args.with_hk and IsHkCode(raw):
            hk_codes.append(raw)
        else:
            a_codes.append(raw)

    print(f"二进制:   {TDX_BIN}")
    print(f"数据库:   {DB}")
    print(f"代码数:   {len(a_codes)} A 股" +
          (f", {len(hk_codes)} 港股（HK 不参与 K 线导入）" if hk_codes else ""))
    if a_codes:
        print(f"示例:     {' '.join(a_codes[:4])}{' ...' if len(a_codes) > 4 else ''}")
    if hk_codes:
        print(f"港股跳过: {' '.join(hk_codes[:6])}{' ...' if len(hk_codes) > 6 else ''}")

    if not a_codes:
        sys.exit("无有效 A 股代码，退出。")

    # 3. 组装 tdx import 命令（代码走位置参数，支持 sh/sz/bj 三市）
    cmd = [TDX_BIN, "import"] + a_codes
    if args.daily_only:
        cmd.append("--daily-only")
    if args.keep_intraday:
        cmd.append("--no-clear-intraday")
    if args.jobs > 0:
        cmd += ["--jobs", str(args.jobs)]
    # 截断显示，避免 16000 个代码刷屏
    shown = " ".join(cmd[:6]) + (f" ... +{len(a_codes)} 个代码" if len(a_codes) > 4 else "")
    print(f"命令:     {shown}")
    print(f"模式:     {'DRY-RUN（不执行）' if args.dry_run else '执行（vipdoc 增量 + 网络补缺）'}")

    if args.dry_run:
        print(f"\n[dry-run] 未执行。将导入 {len(a_codes)} 只 A 股。去掉 --dry-run 正式导入。")
        return

    # 4. 执行
    print(f"\n>>> [append] 开始导入 {len(a_codes)} 只 A 股...")
    t0 = time.perf_counter()
    r = subprocess.run(cmd)
    dt = time.perf_counter() - t0

    if r.returncode != 0:
        sys.exit(f"\n[FAIL] tdx import 退出码={r.returncode}，耗时 {fmt(dt)}。请检查上方日志。")


    # 5.5 czsc 信号落库（import 成功后）
    if args.czsc:
        czsc_cmd = [TDX_BIN, "czsc", "--czsc_freqs", args.czsc_freqs]
        if args.codes:
            czsc_cmd += ["--czsc_codes", ",".join(a_codes)]
        else:
            czsc_cmd.append("--czsc_all")
        if args.czsc_dry:
            czsc_cmd.append("--czsc_dry")
        if args.jobs > 0:
            czsc_cmd += ["--czsc_n", str(args.jobs)]
        print(f"\n>>> [czsc] 信号落库 {len(a_codes)} 只...")
        t1 = time.perf_counter()
        r2 = subprocess.run(czsc_cmd)
        dt2 = time.perf_counter() - t1
        tag = "dry-run" if args.czsc_dry else "已写入"
        print(f"[czsc] {tag} signals 表，耗时 {fmt(dt2)}。")
        if r2.returncode != 0:
            sys.exit(f"[FAIL] tdx czsc 退出码={r2.returncode}。")

    # 5. 汇总行数
    def count(stable):
        out = taos_query(f"SELECT COUNT(*) FROM {stable}")
        for ln in out.splitlines():
            first = ln.split("|")[0].strip()
            if first.isdigit():
                return int(first)
        return -1

    kline_rows = count("kline")
    adj_rows = count("adjust")

    print("\n" + "=" * 50)
    print(f"{'阶段':<16}{'耗时':<22}{'行数':>10}")
    print("-" * 50)
    print(f"{'增量导入':<16}{fmt(dt):<22}{kline_rows:>10}")
    print("-" * 50)
    print(f"kline={kline_rows}  adjust={adj_rows}")
    print("=" * 50)
    print(f"[append] 完成，耗时 {fmt(dt)}。")


if __name__ == "__main__":
    main()
