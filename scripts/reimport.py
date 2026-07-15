#!/usr/bin/env python3
# 清 kline + adjust + finance + f10 表 → 重新导入 → 统计耗时。
#
# 数据流（与项目默认一致）：
#   kline   ← tdx import --full-reset（本地 vipdoc，默认读自选股 zxg.blk）
#   finance ← tdx fetch-finance <codes>（网络 0x10，逐 code 清 fn_<code> 重导）
#   f10     ← tdx fetch-f10 <codes>（网络 0x2cf/0x2d0，逐 code 清 fc/ft_<code>_* 重导）
#
# 清表：DROP STABLE（级联删全部子表）；导入命令自带 CREATE STABLE IF NOT EXISTS 重建。
# kline 用 --full-reset：清表后走 DROP TABLE IF EXISTS（安全 no-op），比默认 clear_intraday
#   的 DELETE FROM <缺失子表> 干净（后者会打印「清除 X 失败」告警）。
# stock_name 表不清（import 靠它过滤 zxg 代码）。
#
# 用法：
#   python3 scripts/reimport.py                  # 默认：自选股 zxg.blk
#   python3 scripts/reimport.py --dry-run        # 只打印计划，不执行
#   python3 scripts/reimport.py --codes sh600000 sz000001
#   python3 scripts/reimport.py --codes-file my_codes.txt
#   python3 scripts/reimport.py --skip-kline     # 跳过某阶段
#
# 环境变量：TDX_BIN  TAOS  TDX_TAOS_DB  TDX_ZXG_BLK
import argparse
import os
import subprocess
import sys
import time

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TDX_BIN = os.environ.get("TDX_BIN", os.path.join(REPO, "build", "bin", "tdx"))
TAOS = os.environ.get("TAOS", "taos")
DB = os.environ.get("TDX_TAOS_DB", "tdx")
ZXG_DEFAULT = "/home/li/.local/share/tdxcfv/drive_c/tc/T0002/blocknew/zxg.blk"

# 与 src/taos/taos_import.cpp::EnsureKlineTables / EnsureFinanceTable / EnsureF10Tables 对齐
STABLES = ["kline", "adjust", "finance", "f10_cat", "f10_text"]


def IsHkCode(code):
    # 复刻 C++ tdx::IsHkCode（include/tdx/consts.hpp）。
    # 保守规则：显式 sh/sz/bj 前缀 → A 股；4-5 位纯数字 / 字母开头 → HK；6-8 位数字不识别。
    if len(code) >= 8 and code[:2] in ("sh", "sz", "bj"):
        return False
    if not code:
        return False
    all_digit = all('0' <= c <= '9' for c in code)
    if all_digit:
        return len(code) in (4, 5)
    return code[0].isalpha()


def read_zxg(path):
    # 逐字复刻 C++ ReadZxgBlk：每行 7 位，首位 1=sh / 0=sz + 6 位代码。
    out = []
    try:
        with open(path, encoding="ascii", errors="ignore") as f:
            for line in f:
                line = line.rstrip("\r\n \t")
                if len(line) < 7:
                    continue
                m = line[0]
                if m not in ("0", "1"):
                    continue
                out.append(("sh" if m == "1" else "sz") + line[1:7])
    except FileNotFoundError:
        pass
    return out


def taos_sql(sql):
    r = subprocess.run([TAOS, "-s", f"USE {DB}; {sql}"], capture_output=True, text=True)
    if r.returncode != 0:
        sys.exit(f"[FAIL] taos SQL 失败: {sql}\n{r.stderr.strip()}")
    return r.stdout


def taos_count(stable):
    # taos CLI 输出 CRLF + '|' 列分隔（如 "       1600936 |"），取首字段判数字。
    # 表可能不存在（跳过某阶段后 STABLE 未重建）→ 吞错返回 -1，不中断汇总。
    r = subprocess.run([TAOS, "-s", f"USE {DB}; SELECT COUNT(*) FROM {stable}"],
                       capture_output=True, text=True)
    for ln in r.stdout.splitlines():
        first = ln.split("|")[0].strip()
        if first.isdigit():
            return int(first)
    return -1  # 表不存在或查询失败


def timed(fn):
    t0 = time.perf_counter()
    fn()
    return time.perf_counter() - t0


def run_cli(label, args, dry, retries=3):
    # fetch-* 的 StdQuotes::Connect 选服有瞬时抖动（全部服务器不可达），重试即好；
    # 三命令均幂等（逐 code 清子表后重写），失败重跑安全。返回 (耗时, 是否成功)。
    print(f"\n>>> [{label}] {' '.join(args)}")
    if dry:
        return 0.0, True
    for attempt in range(1, retries + 1):
        t0 = time.perf_counter()
        r = subprocess.run(args)
        dt = time.perf_counter() - t0
        if r.returncode == 0:
            return dt, True
        print(f"  [第 {attempt}/{retries} 次失败 exit={r.returncode}，{dt:.1f}s]"
              + ("" if attempt < retries else "  放弃"))
        if attempt < retries:
            time.sleep(2 * attempt)  # 退避 2s, 4s
    return None, False


def fmt(secs):
    if secs < 60:
        return f"{secs:.1f}s"
    m, s = divmod(int(secs), 60)
    return f"{secs:.1f}s ({m}m{s}s)"


def main():
    ap = argparse.ArgumentParser(description="清 kline/f10/finance 表 → 重导 → 计时")
    ap.add_argument("--codes", nargs="*", help="显式代码（带 sh/sz 前缀），覆盖 zxg.blk")
    ap.add_argument("--codes-file", help="代码文件，每行一个（# 开头注释）")
    ap.add_argument("--zxg", default=os.environ.get("TDX_ZXG_BLK", ZXG_DEFAULT))
    ap.add_argument("--dry-run", action="store_true", help="只打印计划，不执行")
    ap.add_argument("--all-market", action="store_true",
                    help="kline 导入走全市场（vipdoc 全量，仅影响 kline 阶段）")
    ap.add_argument("--kronos", action="store_true",
                    help="日线清库重建：清库后仅导入个股+大盘指数日K线"
                         "（自动 --all-market --daily-only --kronos，跳过 finance/f10）")
    ap.add_argument("--skip-kline", action="store_true")
    ap.add_argument("--skip-finance", action="store_true")
    ap.add_argument("--skip-f10", action="store_true")
    args = ap.parse_args()

    if not os.path.exists(TDX_BIN):
        sys.exit(f"找不到 tdx 二进制: {TDX_BIN}（可用 TDX_BIN 覆盖）")

    if args.codes:
        codes = args.codes
    elif args.codes_file:
        with open(args.codes_file) as f:
            codes = [ln.split()[0] for ln in f if ln.strip() and not ln.lstrip().startswith("#")]
    else:
        codes = read_zxg(args.zxg)
    # 按 A 股/港股分桶。finance/f10 仅支持 A 股（TDX 协议 0x10/0x2cf/0x2d0 无 HK 实现），
    # HK 码在 fetch-finance/fetch-f10 阶段跳过（kline 也仅 vipdoc/A 股，默认无 HK）。
    # fetch-quotes --quote_hk 才覆盖 HK 实时行情（0x248a）。
    a_codes = []
    hk_codes = []
    for c in codes:
        explicit_a = len(c) >= 8 and c[:2] in ("sh", "sz", "bj")
        if not explicit_a and IsHkCode(c):
            hk_codes.append(c)
        else:
            a_codes.append(c)
    codes = a_codes  # kline/finance/f10 仅 A 股

    print(f"二进制: {TDX_BIN}")
    print(f"代码数: {len(codes)} A股, {len(hk_codes)} 港股（HK 不参与 finance/f10 重导）"
          + (f"   示例: {' '.join(codes[:4] + hk_codes[:2])}" if codes or hk_codes else ""))
    if hk_codes:
        print(f"港股跳过finance/f10: {' '.join(hk_codes[:8])}"
              + (" ..." if len(hk_codes) > 8 else ""))
    if args.kronos:
        print("模式:   日线清库重建（kronos——个股+大盘指数日K线 + 复权因子）")
    else:
        print(f"模式:   {'DRY-RUN（不执行）' if args.dry_run else '执行（清空 5 张超级表并重导）'}")

    res = {}

    # 1. 清表（单一清理点，统一计时）
    def do_clear():
        for s in STABLES:
            taos_sql(f"DROP STABLE IF EXISTS {s}")
            print(f"  DROP STABLE {s}")
    print("\n>>> [clear] " + ", ".join(STABLES))
    res["clear"] = 0.0 if args.dry_run else timed(do_clear)

    # 2. kline（--full-reset：清表后走 DROP TABLE IF EXISTS，干净无告警）
    failed = []
    def phase(key, label, cmd):
        dt, ok = run_cli(label, cmd, args.dry_run)
        if ok:
            res[key] = dt
        else:
            failed.append(key)

    if args.kronos:
        # 日线清库重建：个股+大盘指数日K线（含复权因子），跳过 finance/f10（按 code 操作，与本模式无关）。
        # --kronos 暗含 --all-market --daily-only；复权因子保持开启（不传 --no-adjust）。
        if not args.skip_kline:
            phase("kline", "kline import (kronos)",
                  [TDX_BIN, "import", "--full-reset", "--all-market", "--daily-only", "--kronos"])
    else:
        if not args.skip_kline:
            import_cmd = [TDX_BIN, "import", "--full-reset"]
            if args.all_market:
                import_cmd.append("--all-market")
            phase("kline", "kline import", import_cmd)
        if not args.skip_finance:
            phase("finance", "finance", [TDX_BIN, "fetch-finance"] + codes)
        if not args.skip_f10:
            phase("f10", "f10", [TDX_BIN, "fetch-f10"] + codes)

    # 5. 行数 + 汇总
    counts = {} if args.dry_run else {s: taos_count(s) for s in ("kline", "finance", "f10_cat", "f10_text")}

    print("\n" + "=" * 54)
    print(f"{'阶段':<14}{'耗时':<22}{'行数':>10}")
    print("-" * 54)
    print(f"{'清表':<14}{fmt(res['clear']):<22}{'-':>10}")
    for key, lab, tbl in [("kline", "kline 导入", "kline"),
                          ("finance", "finance", "finance"),
                          ("f10", "f10", "f10_cat")]:
        if key in failed:
            print(f"{lab:<14}{'失败（已重试）':<22}{'-':>10}")
        elif key in res:
            rows = "-" if args.dry_run else counts.get(tbl, "-")
            print(f"{lab:<14}{fmt(res[key]):<22}{rows:>10}")
    total = sum(res[k] for k in ("kline", "finance", "f10") if k in res)
    print("-" * 54)
    print(f"{'重导合计':<14}{fmt(total):<22}")
    if counts:
        print(f"kline={counts['kline']}  finance={counts['finance']}  "
              f"f10_cat={counts['f10_cat']}  f10_text={counts['f10_text']}")
    print("=" * 54)


if __name__ == "__main__":
    main()
