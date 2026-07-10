#!/usr/bin/env python3
# 编排：N × fetch-kline（当日K线入库）+ 1 × fetch-quotes（行情 → shm + TDengine），屏幕定时打印行情。
#
# 数据源：
#   - fetch-quotes 单进程单写者（shm flock 排他）+ 内部 N 路 helio fiber 多线程拉取（--quote_jobs，默认 8）。
#     shm 行情由 fetch-quotes --mmap_path 落盘；同时经异步 ingest worker 入库 TDengine。
#   - fetch-kline：round-robin 把 codes 切到 N 进程 (--kline-jobs，默认 1)，各持独立 opentdx/Taos
#     连接循环 upsert 当日 K 线（无共享状态，upsert 幂等不冲突）。
#
# 两种输出模式（由 --mmap 切换；shm 段始终开启）：
#   A) viewer 模式（--mmap [path]）
#       全屏 TUI（ShmViewer 读 shm），stdout；进度/子进程 stderr → dup2 到日志文件。
#   B) 默认模式（无 --mmap）
#       紧凑行情表（tailboard 读 shm）写 stdout；轮次/进度/子进程 stderr → dup2 到日志文件。
#
# 设计要点（屏幕 / 日志互不污染）：
#   - 子进程 stdout 重定向到 DEVNULL，stderr = 被 dup2 后的日志 fd；进度轮次只写 stderr
#   - 本进程 stderr 在 shm 模式下经 dup2 重定向到日志文件，stdout 只写行情（tailboard/TUI）
#   - 子进程 stderr 经行缓冲临时文件（Linux 下 stdbuf -eL 强制行缓冲，防 abort 丢 trace）；
#     父进程用 mkstemp fd 传入 stderr 后立即 os.close，无双重 fd、无泄漏
#   - 进度报告读内存环形缓冲 deque，不阻塞子进程
#   - 父子同进程组：Ctrl-C 被子进程 tdx 自行捕获优雅退出；父进程仅等待 8s grace，
#     超时才补发 terminate()，避免与子进程自有清理竞争
#
# 用法：
#   python3 scripts/fetch-today.py                              # 默认：行情表上屏，进度入日志
#   python3 scripts/fetch-today.py --mmap                       # 全屏 TUI
#   python3 scripts/fetch-today.py --codes sh600000 sz000001
#   python3 scripts/fetch-today.py --codes-file my.txt
#   python3 scripts/fetch-today.py --kline-jobs 4 --quote-jobs 8
#   python3 scripts/fetch-today.py --interval 30
#   python3 scripts/fetch-today.py --no-shm                     # 仅入库，不启 shm / 屏幕行情
#
# 环境变量：TDX_BIN  TDX_ZXG_BLK  TDX_SHM  REPORT_INTERVAL
import argparse
import atexit
import collections
import mmap
import os
import shutil
import signal
import struct
import subprocess
import sys
import tempfile
import threading
import time

DEFAULT_ZXG = "/home/li/.local/share/tdxcfv/drive_c/tc/T0002/blocknew/zxg.blk"
DEFAULT_SHM = "/dev/shm/tdx_quotes.shm"

# ---- shm 二进制布局（与 include/tdx/shm/{segment,snapshot,payload}.hpp 严格一致） ----
HDR_OFF_MAGIC = 0             # 'TDXSHM\0\0'
HDR_OFF_VERSION = 8           # u32, must == 1
HDR_OFF_SNAP_SLOTS = 56       # u32, power of 2
HDR_OFF_SNAP_SLOT_SIZE = 60   # u32
HDR_OFF_SNAP_OFF = 64         # u64
# SnapSlot 256B alignas(64): seq(u64) + code(8s) + QuotePOD(224) + flags(u32) + pad(12)
SLOT_SIZE = 256
SLOT_OFF_SEQ = 0
SLOT_OFF_CODE = 8
SLOT_OFF_POD = 16
SLOT_OFF_FLAGS = 240
# QuotePOD 224B: datetime(i64) + price,pre_close,open,high,low,volume,amount (7×f64) + bid5,ask5,bvol5,avel5 (20×f64)
POD_FMT = "< q 27d"
assert struct.calcsize(POD_FMT) == 224, "QuotePOD 必须 224B"


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


def round_robin(items, n):
    """round-robin 切片：保持顺序，近似均匀。n<=1 返回 [items]。"""
    n = max(1, int(n))
    if n == 1:
        return [list(items)]
    buckets = [[] for _ in range(n)]
    for i, x in enumerate(items):
        buckets[i % n].append(x)
    return [b for b in buckets if b]


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
        if sys.platform == "linux" and shutil.which("stdbuf"):
            self.cmd = ["stdbuf", "-eL"] + self.cmd

    def start(self):
        fd, self._err_path = tempfile.mkstemp(
            prefix=f"fetch_today_{self.name.strip()}_", suffix=".log")
        self.proc = subprocess.Popen(
            self.cmd, stdout=subprocess.DEVNULL, stderr=fd)
        os.close(fd)
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


def wait_shm_ready(shm, proc, timeout=90):
    deadline = time.time() + timeout
    while time.time() < deadline:
        if os.path.exists(shm):
            break
        if proc.poll() is not None:
            return False
        time.sleep(0.2)
    if not os.path.exists(shm):
        return False
    deadline2 = time.time() + 30
    while time.time() < deadline2:
        if proc.poll() is not None:
            return False
        try:
            if os.path.getsize(shm) > 4096 + 100:
                return True
        except OSError:
            pass
        time.sleep(0.5)
    return False


class ReadShm:
    """只读开 seqlock 快照表 + 按 code 读取（镜像 SnapshotTable::Get）。"""

    def __init__(self, shm_path):
        self.shm_path = shm_path
        self._fd = None
        self._mm = None
        self._slots = 0
        self._snap_off = 0
        self._slot_size = 0

    def open(self):
        try:
            fd = os.open(self.shm_path, os.O_RDONLY)
        except OSError:
            return False
        st = os.fstat(fd)
        if st.st_size < 1024:
            os.close(fd)
            return False
        mm = mmap.mmap(fd, 0, access=mmap.ACCESS_READ)
        magic = mm[HDR_OFF_MAGIC:HDR_OFF_MAGIC + 8]
        version = struct.unpack_from("< I", mm, HDR_OFF_VERSION)[0]
        if magic != b"TDXSHM\0\0" or version != 1:
            mm.close(); os.close(fd); return False
        snap_slots, snap_slot_size = struct.unpack_from("< II", mm, HDR_OFF_SNAP_SLOTS)
        snap_off = struct.unpack_from("< Q", mm, HDR_OFF_SNAP_OFF)[0]
        if snap_off + int(snap_slots) * snap_slot_size > st.st_size:
            mm.close(); os.close(fd); return False
        self._fd, self._mm = fd, mm
        self._slots, self._snap_off, self._slot_size = snap_slots, snap_off, snap_slot_size
        return True

    def close(self):
        if self._mm is not None:
            self._mm.close()
        if self._fd is not None:
            os.close(self._fd)
        self._mm = self._fd = None

    def _lookup(self, code):
        if not code:
            return None
        bs = code if isinstance(code, bytes) else code.encode("ascii")
        if len(bs) > 7:
            return None
        mask = self._slots - 1
        h = 2166136261
        for c in bs:
            h = ((h ^ c) * 16777619) & 0xFFFFFFFF
        idx0 = h & mask
        mm = self._mm
        snap_off = self._snap_off
        slot_size = self._slot_size
        for _retries in range(8):
            idx = idx0
            for _probe in range(self._slots):
                off = snap_off + idx * slot_size
                s1 = struct.unpack_from("< Q", mm, off + SLOT_OFF_SEQ)[0]
                if s1 & 1:
                    break  # 写中 → 本轮重试
                block_c = mm[off + SLOT_OFF_CODE:off + SLOT_OFF_CODE + 8]
                pod = struct.unpack_from(POD_FMT, mm, off + SLOT_OFF_POD)
                s2 = struct.unpack_from("< Q", mm, off + SLOT_OFF_SEQ)[0]
                if s1 != s2:
                    break  # 撕裂 → 本轮重试
                if block_c[0] == 0:
                    return None
                end = block_c.find(b"\x00")
                if end < 0:
                    end = 8
                if end == len(bs) and block_c[:end] == bs:
                    return pod
                idx = (idx + 1) & mask
        return None


def _row_from_pod(code, pod):
    ts_str = "--:--:--"
    if pod is None:
        return f" {code:<8s} {ts_str:>8s} (无数据)"
    try:
        ts_str = time.strftime("%H:%M:%S", time.localtime(pod[0]))
    except (OSError, OverflowError, ValueError):
        ts_str = "--:--:--"
    price, pre_close = pod[1], pod[2]
    open_, high, volume, amount = pod[3], pod[4], pod[6], pod[7]
    chg_pct = 0.0
    if pre_close > 0:
        chg_pct = (price - pre_close) / pre_close * 100.0
    color, reset = "", ""
    if chg_pct > 0.01:
        color, reset = "\033[31m", "\033[0m"
    elif chg_pct < -0.01:
        color, reset = "\033[32m", "\033[0m"
    return (
        f" {color}{code:<8s}{reset}"
        f" {ts_str:>8s}"
        f" {price:8.2f} {pre_close:8.2f} {open_:8.2f} {high:8.2f}"
        f" {volume:12.0f} {amount:13.0f} {chg_pct:5.1f}%"
    )


TAILBOARD_HDR = (
    f" {'代码':<8s} {'时间':<8s} {'现价':<8s} {'昨收':<8s} {'开盘':<8s} {'最高':<8s}"
    f" {'成交量':<12s} {'成交额':<13s} {'涨跌%':<6s}"
)


def tailboard_render(shm, viewer_codes):
    """读 shm + 渲染紧凑行情表字符串（不含 ANSI 清屏，仅表内容）。"""
    rd = ReadShm(shm)
    if not rd.open():
        return "[行情] 尚未就绪（writer 未落盘或正在选服...）", False
    try:
        rows = TAILBOARD_HDR + "\n" + ("-" * 98) + "\n"
        rows += "\n".join(_row_from_pod(c, rd._lookup(c)) for c in viewer_codes)
        return rows, True
    finally:
        rd.close()


class ShmViewer:
    """内嵌 Python 全屏幕 TUI：阻塞直到 Ctrl-C / 关闭。"""

    def __init__(self, shm_path, codes, interval=3):
        self.shm_path = shm_path
        self.codes = codes
        self.interval = interval

    def run(self):
        print(
            f"\n === 自选股实时行情  (刷新 {self.interval}s, Ctrl-C 退出) ===\n"
            f"{TAILBOARD_HDR}\n{'-' * 98}",
            file=sys.stdout,
        )
        try:
            while True:
                body, _ = tailboard_render(self.shm_path, self.codes)
                sys.stdout.write("\033[2J\033[H")
                sys.stdout.write(
                    f" === 自选股实时行情  (刷新 {self.interval}s, Ctrl-C 退出) ===\n"
                    f"{body}\n"
                )
                sys.stdout.flush()
                time.sleep(self.interval)
        except (KeyboardInterrupt, BrokenPipeError):
            pass
        return True


def main():
    ap = argparse.ArgumentParser(
        description="编排 N×fetch-kline + fetch-quotes(shm+TDengine)；屏幕行情，日志入 stderr")
    ap.add_argument("--codes", nargs="*", default=None)
    ap.add_argument("--codes-file", default=None)
    ap.add_argument("--bin", default=os.environ.get("TDX_BIN", "build/bin/tdx"))
    ap.add_argument("--zxg", default=os.environ.get("TDX_ZXG_BLK", DEFAULT_ZXG))
    ap.add_argument("--mmap", nargs="?", const="",
                    default=None,
                    help="启用 viewer 模式（全屏 TUI）。可选 shm 路径；空 = 默认 /dev/shm/tdx_quotes.shm")
    ap.add_argument("--shm", default=os.environ.get("TDX_SHM", DEFAULT_SHM),
                    help="shm 路径（默认 /dev/shm/tdx_quotes.shm；可区分多实例）")
    ap.add_argument("--interval", type=int,
                    default=int(os.environ.get("REPORT_INTERVAL", "60")),
                    help="报告/刷新间隔（默认 60s）")
    ap.add_argument("--kline-jobs", type=int, default=1,
                    help="fetch-kline 并行进程数（round-robin 切 codes，默认 1）")
    ap.add_argument("--quote-jobs", type=int, default=8,
                    help="fetch-quotes 写数（写入 shm + TDengine 异步入库）。"
                         "viewer: 1 写者 + --quote_jobs=N 内部 fiber"
                         "all use default 8 .")
    args = ap.parse_args()

    if args.codes:
        codes = args.codes
    elif args.codes_file:
        with open(args.codes_file) as f:
            codes = f.read().split()
    else:
        codes = read_zxg(args.zxg)
    if not codes:
        sys.stderr.write("无代码（--codes / --codes-file / zxg.blk 均空）\n")
        return 1
    codes = [c for c in codes if len(c) >= 6]

    # 解析：--mmap（viewer 全屏 TUI）, --shm（日志/屏幕行情流所用的 shm 路径）
    # args.mmap == None → 未传 → 默认模式（屏幕紧凑行情表）
    # args.mmap == ''   → 传了 --mmap 无值 → viewer 模式，shm 默认
    # args.mmap == path → viewer 模式，指定 shm
    if args.mmap is None:
        viewer_mode, shm = False, args.shm
    else:
        viewer_mode, shm = True, (args.mmap or args.shm)

    if not shutil.which(args.bin):
        sys.stderr.write(f"找不到 tdx 二进制: {args.bin}\n")
        return 1

    kline_jobs = max(1, int(args.kline_jobs))
    quote_jobs = max(1, int(args.quote_jobs))

    kline_shards = round_robin(codes, kline_jobs)
    kline_monitors = [ProcMonitor(f"kline#{i}", [args.bin, "fetch-kline"] + shard)
                      for i, shard in enumerate(kline_shards)]

    # shm 键为 6 位 code（blk 剥前缀后即是；TDX 现价直接存放元为单位的 double）
    viewer_codes = [c[2:] if c[:2] in ("sh", "sz", "bj") else c for c in codes]
    viewer_codes = [c for c in viewer_codes if len(c) == 6]

    # fetch-quotes：1 写者 + 内部 --quote_jobs 路 fiber；写 shm + 异步 ingest TDengine
    quotes_cmd = ([args.bin, "fetch-quotes", "--quote_loop"]
                  + ["--quote_codes=" + ",".join(codes),
                     "--quote_jobs", str(quote_jobs),
                     "--mmap_path", shm])
    quotes_monitors = [ProcMonitor("quotes", quotes_cmd)]
    monitors = kline_monitors + quotes_monitors
    stop = threading.Event()

    # stderr 落到日志文件（屏幕只输出 stdout：尾行行情 TUI）。
    log_path = os.path.join(os.path.dirname(shm) or "/dev/shm", "fetch_today.log")
    log = open(log_path, "a")
    os.dup2(log.fileno(), sys.stderr.fileno())

    sys.stderr.write(
        f"=== fetch-today: {len(codes)} 只 | bin={args.bin} | shm={shm} | "
        f"kline-jobs={kline_jobs} | quote-jobs={quote_jobs} | viewer={viewer_mode} | "
        f"报告间隔 {args.interval}s | 日志={log_path} ===\n")

    def report_loop():
        while not stop.wait(args.interval):
            ts = time.strftime("%H:%M:%S")
            lines = []
            for m in monitors:
                line, dead = m.status()
                lines.append(f"  [{m.name}]{'[退出]' if dead else ''} {line}")
            sys.stderr.write(f"\n[{ts}] --- 进度 ---\n" + "\n".join(lines) + "\n")
            # stdout 仅行情表；viewer 模式已由全屏 TUI 占用 stdout，不再重复打印
            if viewer_mode:
                continue
            board, _ = tailboard_render(shm, viewer_codes)
            sys.stdout.write("\033[2J\033[H" + board + "\n")
            sys.stdout.flush()

    threading.Thread(target=report_loop, daemon=True).start()

    def cleanup():
        for m in monitors:
            m.stop()

    def on_sig(signum, frame):
        sys.stderr.write("\n收到信号，等待子进程优雅退出（最多 8s）...\n")
        stop.set()
        deadline = time.time() + 8
        while time.time() < deadline:
            if all(m.proc is not None and m.proc.poll() is not None for m in monitors):
                break
            time.sleep(0.2)
        cleanup()
        sys.exit(0)

    signal.signal(signal.SIGINT, on_sig)
    signal.signal(signal.SIGTERM, on_sig)

    for m in monitors:
        m.start()

    quotes_monitor = quotes_monitors[0]
    sys.stderr.write(f"等待 shm 就绪（writer 落盘 {shm} + 至少一轮采集）...\n")
    if not wait_shm_ready(shm, quotes_monitor.proc):
        rc = quotes_monitor.proc.poll() if quotes_monitor.proc else None
        sys.stderr.write(f"writer 未就绪（rc={rc}），退出\n")
        return 1
    sys.stderr.write(f"shm 就绪（{os.path.getsize(shm)} B），{'拉起全屏 TUI' if viewer_mode else '开始行情'}\n")

    try:
        if viewer_mode:
            ShmViewer(shm, viewer_codes, interval=args.interval).run()
        else:
            while not stop.wait(0.5):
                if any(m.proc is not None and m.proc.poll() is not None for m in monitors):
                    break
        cleanup()
        return 0
    except KeyboardInterrupt:
        cleanup()
        return 130


if __name__ == "__main__":
    sys.exit(main())
