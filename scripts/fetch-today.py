#!/usr/bin/env python3
# 编排：并行启动 fetch-kline（当日K线入库）+ fetch-quotes（实时行情），定时报告进度。
#
# 数据流：
#   fetch-kline  ← tdx fetch-kline <codes>（当日 1d/5m/1m 循环刷新入库；收盘后 3 轮无新 bar 自退）
#   fetch-quotes ← tdx fetch-quotes --quote_loop（实时行情→TDengine，默认自选股 zxg.blk）
#
# 关键设计（互不干扰 / 不丢日志）：
#   - kline 与 quotes 为独立子进程，各自独立 helio ProactorPool / TaosConnection，互不占用
#   - 子进程 stderr 经行缓冲临时文件（Linux 下 stdbuf -eL 强制行缓冲，防 abort 丢 trace）；
#     父进程用 mkstemp fd 传入 stderr 后立即 os.close，无双重 fd、无泄漏
#   - 进度报告读内存环形缓冲 deque，不阻塞子进程
#   - 父子同进程组：Ctrl-C 被子进程 tdx 自行捕获优雅退出；父进程仅等待 8s grace，
#     超时才补发 terminate()，避免与子进程自有清理竞争
#
# 用法：
#   python3 scripts/fetch-today.py                        # 默认 zxg.blk，每 60s 报告
#   python3 scripts/fetch-today.py --codes sh600000 sz000001
#   python3 scripts/fetch-today.py --codes-file my.txt
#   python3 scripts/fetch-today.py --mmap /dev/shm/tdx    # fetch-quotes 启用 mmap 快照
#   python3 scripts/fetch-today.py --interval 30
#
# 环境变量：TDX_BIN  TDX_ZXG_BLK  REPORT_INTERVAL  MMAP_PATH
import argparse
import atexit
import collections
import os
import shutil
import signal
import subprocess
import sys
import tempfile
import threading
import time

DEFAULT_ZXG = "/home/li/.local/share/tdxcfv/drive_c/tc/T0002/blocknew/zxg.blk"


def read_zxg(path):
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
    """子进程监控（行缓冲 stderr 文件 + 内存环形缓冲）。"""

    def __init__(self, name, cmd):
        self.name = name
        self.cmd = list(cmd)
        self.proc = None
        self.last_line = ""
        self.lock = threading.Lock()
        self._deque = collections.deque(maxlen=500)  # [(timestamp, line), ...]
        self._err_path = None
        # best-effort 强制 C++ 子进程 stderr 行缓冲（防 abort 丢 trace；不可用时静默跳过）
        if sys.platform == "linux" and shutil.which("stdbuf"):
            self.cmd = ["stdbuf", "-eL"] + self.cmd

    def start(self):
        fd, self._err_path = tempfile.mkstemp(
            prefix=f"fetch_today_{self.name.strip()}_", suffix=".log")
        self.proc = subprocess.Popen(
            self.cmd, stdout=subprocess.DEVNULL, stderr=fd)
        os.close(fd)                     # 子进程已 dup，父进程不持有写入端 → 无双重 fd
        atexit.register(self._cleanup)
        threading.Thread(target=self._tail, daemon=True).start()

    def _tail(self):
        try:
            with open(self._err_path, "r", errors="replace") as f:
                while True:
                    line = f.readline()
                    if line:
                        line = line.rstrip()
                        if line:
                            item = (time.strftime("%H:%M:%S"), line)
                            with self.lock:
                                self.last_line = line
                                self._deque.append(item)
                    elif self.proc.poll() is not None:
                        break
                    else:
                        time.sleep(0.1)
        except Exception:
            pass

    def status(self):
        with self.lock:
            return self.last_line, (self.proc.poll() is not None)

    def dump_stderr(self, tail=50):
        """异常退出时优先 dump 环形缓冲；为空则回退读文件。"""
        with self.lock:
            items = list(self._deque)
        if items:
            start = max(0, len(items) - tail)
            return "".join(f"[{t}] {line}\n" for t, line in items[start:])
        try:
            with open(self._err_path, errors="replace") as f:
                lines = f.readlines()[-tail:]
            return "".join(lines)
        except Exception:
            return ""

    def _cleanup(self):
        if self._err_path and os.path.exists(self._err_path):
            try:
                os.unlink(self._err_path)
            except OSError:
                pass

    def stop(self):
        if self.proc and self.proc.poll() is None:
            self.proc.terminate()


def main():
    ap = argparse.ArgumentParser(description="编排 fetch-kline + fetch-quotes，定时报告进度")
    ap.add_argument("--codes", nargs="*", default=None)
    ap.add_argument("--codes-file", default=None)
    ap.add_argument("--bin", default=os.environ.get("TDX_BIN", "build/bin/tdx"))
    ap.add_argument("--zxg", default=os.environ.get("TDX_ZXG_BLK", DEFAULT_ZXG))
    ap.add_argument("--mmap", default=os.environ.get("MMAP_PATH", ""))
    ap.add_argument("--interval", type=int,
                    default=int(os.environ.get("REPORT_INTERVAL", "60")))
    args = ap.parse_args()

    if args.codes:
        codes = args.codes
    elif args.codes_file:
        with open(args.codes_file) as f:
            codes = f.read().split()          # 支持每行一个与空格分隔（单 / 多行）
    else:
        codes = read_zxg(args.zxg)
    if not codes:
        sys.stderr.write("无代码（--codes / --codes-file / zxg.blk 均空）\n")
        return 1

    sys.stderr.write(f"=== fetch-today: {len(codes)} 只 | bin={args.bin} | "
                     f"报告间隔 {args.interval}s ===\n")

    kline_cmd = [args.bin, "fetch-kline"] + codes
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
        while not stop.wait(args.interval):
            ts = time.strftime("%H:%M:%S")
            lines = []
            for m in monitors:
                line, dead = m.status()
                lines.append(f"  [{m.name}]{'[退出]' if dead else ''} {line}")
            sys.stderr.write(f"\n[{ts}] --- 进度 ---\n" + "\n".join(lines) + "\n")

    threading.Thread(target=report_loop, daemon=True).start()

    def on_sig(signum, frame):
        sys.stderr.write("\n收到信号，等待子进程优雅退出（最多 8s）...\n")
        stop.set()
        deadline = time.time() + 8
        # 等子进程自行优雅退出；超时未退出者补 terminate()
        while time.time() < deadline:
            if all(m.proc.poll() is not None for m in monitors):
                break
            time.sleep(0.2)
        for m in monitors:
            m.stop()
        sys.exit(0)

    signal.signal(signal.SIGINT, on_sig)
    signal.signal(signal.SIGTERM, on_sig)

    for m in monitors:
        m.start()

    while True:
        for m in monitors:
            rc = m.proc.poll()
            if rc is not None:
                stop.set()
                sys.stderr.write(f"\n[{m.name.strip()}] 进程退出 (rc={rc})\n")
                if rc != 0:
                    trace = m.dump_stderr()
                    if trace:
                        sys.stderr.write(f"--- {m.name.strip()} stderr 末尾 ---\n{trace}\n")
                # 等待另一进程也退出
                for o in monitors:
                    if o.proc.poll() is None:
                        try:
                            o.proc.wait(timeout=8)
                        except subprocess.TimeoutExpired:
                            o.stop()
                return 0 if rc == 0 else 1
        time.sleep(1)


if __name__ == "__main__":
    sys.exit(main())