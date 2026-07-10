#!/usr/bin/env python3
# 编排：并行启动 fetch-kline（当日K线入库）+ fetch-quotes（实时行情），每分钟报告进度。
#
# 数据流：
#   fetch-kline  ← tdx fetch-kline <codes>（当日 1d/5m/1m 循环刷新入库；收盘后 3 轮无新 bar 自动退出）
#   fetch-quotes ← tdx fetch-quotes --quote_loop（实时行情→TDengine，默认自选股 zxg.blk）
#
# codes 来源（fetch-kline 位置参数，须 sh/sz 前缀）：
#   --codes 显式 > --codes-file > zxg.blk（与 fetch-quotes 默认采集范围一致）
# 显式 --codes 时同步传 fetch-quotes --quote_codes；否则 fetch-quotes 自读 zxg.blk。
#
# 用法：
#   python3 scripts/fetch-today.py                        # 默认 zxg.blk
#   python3 scripts/fetch-today.py --codes sh600000 sz000001
#   python3 scripts/fetch-today.py --codes-file my.txt
#   python3 scripts/fetch-today.py --mmap /dev/shm/tdx    # fetch-quotes 启用 mmap 快照
#   python3 scripts/fetch-today.py --interval 30          # 报告间隔（秒）
#
# 环境变量：TDX_BIN  TDX_ZXG_BLK  REPORT_INTERVAL  MMAP_PATH
# 任一子进程退出（含 fetch-kline 收盘自动退出）则终止全部并退出。Ctrl-C 优雅终止。
import argparse
import os
import signal
import subprocess
import sys
import threading
import time

DEFAULT_ZXG = "/home/li/.local/share/tdxcfv/drive_c/tc/T0002/blocknew/zxg.blk"


def read_zxg(path):
    """读 zxg.blk：每行 7 位（首位 1=沪 sh / 0=深 sz + 6 位代码）→ 带 sh/sz 前缀。"""
    codes = []
    try:
        with open(path) as f:
            for line in f:
                line = line.strip()
                if len(line) < 7 or line[0] not in "01":
                    continue
                codes.append(("sh" if line[0] == "1" else "sz") + line[1:7])
    except FileNotFoundError:
        pass
    return codes


class ProcMonitor:
    """子进程 + stderr 最后一行捕获（供进度报告）。"""

    def __init__(self, name, cmd):
        self.name = name
        self.cmd = cmd
        self.proc = None
        self.last_line = ""
        self.lock = threading.Lock()

    def start(self):
        self.proc = subprocess.Popen(
            self.cmd, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE,
            text=True, bufsize=1)
        threading.Thread(target=self._pump, daemon=True).start()

    def _pump(self):
        for line in self.proc.stderr:
            line = line.rstrip()
            if line:
                with self.lock:
                    self.last_line = line

    def status(self):
        with self.lock:
            return self.last_line, (self.proc.poll() is not None)

    def stop(self):
        if self.proc and self.proc.poll() is None:
            self.proc.terminate()


def main():
    ap = argparse.ArgumentParser(description="编排 fetch-kline + fetch-quotes，定时报告进度")
    ap.add_argument("--codes", nargs="*", default=None,
                    help="显式代码（sh/sz 前缀），同步传 fetch-quotes --quote_codes")
    ap.add_argument("--codes-file", default=None, help="代码文件（每行一个）")
    ap.add_argument("--bin", default=os.environ.get("TDX_BIN", "build/bin/tdx"))
    ap.add_argument("--zxg", default=os.environ.get("TDX_ZXG_BLK", DEFAULT_ZXG))
    ap.add_argument("--mmap", default=os.environ.get("MMAP_PATH", ""),
                    help="fetch-quotes --mmap_path（启用共享内存快照）")
    ap.add_argument("--interval", type=int,
                    default=int(os.environ.get("REPORT_INTERVAL", "60")),
                    help="报告间隔秒（默认 60）")
    args = ap.parse_args()

    # codes 解析：--codes > --codes-file > zxg.blk
    if args.codes:
        codes = args.codes
    elif args.codes_file:
        with open(args.codes_file) as f:
            codes = [l.strip() for l in f if l.strip()]
    else:
        codes = read_zxg(args.zxg)
    if not codes:
        sys.stderr.write("无代码（--codes / --codes-file / zxg.blk 均空）\n")
        return 1

    sys.stderr.write(f"=== fetch-today: {len(codes)} 只 | bin={args.bin} | "
                     f"报告间隔 {args.interval}s ===\n")

    # fetch-kline：codes 作位置参数（默认循环 60s，收盘后 3 轮无新 bar 退出）
    kline_cmd = [args.bin, "fetch-kline"] + codes
    # fetch-quotes：循环模式；显式 codes 时传 --quote_codes，否则自读 zxg.blk
    quotes_cmd = [args.bin, "fetch-quotes", "--quote_loop"]
    if args.codes:
        quotes_cmd.append("--quote_codes=" + ",".join(codes))
    if args.mmap:
        quotes_cmd += ["--mmap_path", args.mmap]

    monitors = [
        ProcMonitor("kline ", kline_cmd),
        ProcMonitor("quotes", quotes_cmd),
    ]

    stop = threading.Event()

    def report_loop():
        # 每 interval 秒打印两进程 stderr 最后一行
        while not stop.wait(args.interval):
            ts = time.strftime("%H:%M:%S")
            lines = []
            for m in monitors:
                line, dead = m.status()
                lines.append(f"  [{m.name}]{'[退出]' if dead else ''} {line}")
            sys.stderr.write(f"\n[{ts}] --- 进度 ---\n" + "\n".join(lines) + "\n")

    threading.Thread(target=report_loop, daemon=True).start()

    def on_sig(signum, frame):
        stop.set()
        sys.stderr.write("\n收到信号，终止子进程...\n")
        for m in monitors:
            m.stop()
        sys.exit(0)

    signal.signal(signal.SIGINT, on_sig)
    signal.signal(signal.SIGTERM, on_sig)

    for m in monitors:
        m.start()

    # 任一子进程退出 → 终止全部（fetch-kline 收盘自动退出会触发）
    while True:
        for m in monitors:
            rc = m.proc.poll()
            if rc is not None:
                stop.set()
                sys.stderr.write(f"\n[{m.name.strip()}] 进程退出 (rc={rc})，终止全部\n")
                for o in monitors:
                    o.stop()
                return 0 if rc == 0 else 1
        time.sleep(1)


if __name__ == "__main__":
    sys.exit(main())
