#!/usr/bin/env python3
# view.py：一键启动 mmap 实时行情查看器（必要时自动拉起 fetch-quotes --mmap_path）。
#
# 数据流：
#   写端：tdx fetch-quotes --mmap_path <shm> --quote_loop --quote_interval 10
#          └─ 盘中行情→/dev/shm 快照段（每轮 ~2s + 10s 间隔）
#   读端：mmap_viewer --shm <shm>（挂 /dev/shm 只读快照，3s 刷新）
#
# 启动顺序：
#   1) 解析参数（blk 自选股文件 / shm 路径）；
#   2) 用 tdx fetch-quotes --mmap_path 自动采集，确保 shm 段文件就绪；
#   3) 拉起 mmap_viewer 进入 3s 刷新循环。
# 退出：Ctrl-C 先停 viewer，随后 terminate writer。
#
# 用法：
#   python3 scripts/view.py                                   # 默认 shm + zxg.blk
#   python3 scripts/view.py --shm /dev/shm/my_quotes.shm
#   python3 scripts/view.py --blk /path/to/zxg.blk
#   python3 scripts/view.py --interval 10                     # writer 采集间隔
# 环境变量：TDX_BIN  TDX_ZXG_BLK  MMAP_PATH  REPORT_INTERVAL
import argparse
import os
import shutil
import signal
import subprocess
import sys
import time

DEFAULT_SHM = "/dev/shm/tdx_quotes.shm"
DEFAULT_ZXG = "/home/li/.local/share/tdxcfv/drive_c/tc/T0002/blocknew/zxg.blk"
VIEWER = "build/bin/mmap_viewer"


def main():
    ap = argparse.ArgumentParser(description="一键启动 mmap 实时行情查看器")
    ap.add_argument("--shm", default=os.environ.get("MMAP_PATH", DEFAULT_SHM),
                    help=f"共享内存文件路径（默认 {DEFAULT_SHM}）")
    ap.add_argument("--blk", default=os.environ.get("TDX_ZXG_BLK", DEFAULT_ZXG),
                    help=f"通达信自选股文件（默认 {DEFAULT_ZXG}）")
    ap.add_argument("--interval", type=int, default=int(os.environ.get("QUOTES_INTERVAL", "10")),
                    help="fetch-quotes 采集间隔秒（默认 10）")
    ap.add_argument("--bin", default=os.environ.get("TDX_BIN", "build/bin/tdx"),
                    help="tdx 二进制路径")
    ap.add_argument("--jobs", type=int, default=8, help="fetch-quotes worker 并发（默认 8）")
    ap.add_argument("--mmap-viewer", default=VIEWER, help="mmap_viewer 二进制路径")
    args = ap.parse_args()

    if not os.path.exists(args.mmap_viewer):
        sys.stderr.write(f"找不到 mmap_viewer: {args.mmap_viewer}\n")
        return 1
    shutil.which(args.bin)  # 尽早发现，不影响已存在时的语义

    shm = args.shm
    sys.stderr.write(
        f"=== view: shm={shm} | blk={args.blk} | writer 间隔 {args.interval}s ===\n")

    # ---------- 起 writer（后台） ----------
    writer_cmd = [args.bin, "fetch-quotes",
                  "--mmap_path", shm,
                  "--quote_loop",
                  "--quote_interval", str(args.interval),
                  "--quote_jobs", str(args.jobs)]
    sys.stderr.write(f"[writer] 启动：{' '.join(writer_cmd)}\n")
    writer = subprocess.Popen(
        writer_cmd, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, text=True)

    # 等待 shm 段文件出现；每 1s 吐一行 writer stderr 当进度（含"第N轮"/选服）
    def relay_writer_log():
        while True:
            line = writer.stderr.readline()
            if not line:
                if writer.poll() is not None:
                    break
                continue
            sys.stderr.write(f"  [writer] {line.rstrip()}\n")

    import threading
    t = threading.Thread(target=relay_writer_log, daemon=True)
    t.start()

    # 等 writer 落盘 shm 段（最多 60s，覆盖冷启动选服+首采延时）
    deadline = time.time() + 60
    while time.time() < deadline:
        if os.path.exists(shm):
            break
        if writer.poll() is not None:
            sys.stderr.write(f"[writer] 提前退出 rc={writer.returncode}，无法启动 viewer\n")
            return 1
        time.sleep(0.2)
    if not os.path.exists(shm):
        sys.stderr.write(f"[writer] 未在 60s 内创建 {shm}\n")
        writer.terminate()
        return 1

    # 再等 writer 报告"第N轮"出现——确保至少一轮采集已写入
    deadline2 = time.time() + 30
    writer_ok = False
    while time.time() < deadline2:
        if writer.poll() is not None:
            break
        # 简单检测：文件非空（首采后段已填充）
        try:
            if os.path.getsize(shm) > 4096 + 100:
                writer_ok = True
                break
        except OSError:
            pass
        time.sleep(0.5)
    sys.stderr.write(f"shm 就绪（{os.path.getsize(shm)} B），拉起 viewer\n")

    # ---------- 起 viewer（前台，3s 刷新循环） ----------
    viewer_cmd = [args.mmap_viewer, "--shm", shm, "--blk", args.blk]
    viewer = subprocess.Popen(viewer_cmd)

    def on_sig(signum, frame):
        sys.stderr.write("\n收到信号，依次停止 viewer → writer...\n")
        if viewer.poll() is None:
            viewer.terminate()
            try:
                viewer.wait(timeout=5)
            except subprocess.TimeoutExpired:
                viewer.kill()
        if writer.poll() is None:
            writer.terminate()
            try:
                writer.wait(timeout=5)
            except subprocess.TimeoutExpired:
                writer.kill()
        sys.exit(0)

    signal.signal(signal.SIGINT, on_sig)
    signal.signal(signal.SIGTERM, on_sig)

    # 子进程托管：哪边先退出就停另一边 + 收尾
    try:
        while True:
            if viewer.poll() is not None:
                break
            if writer.poll() is not None:
                sys.stderr.write(f"\n[writer] 异常退出 rc={writer.returncode}，停止 viewer\n")
                break
            time.sleep(0.5)
    finally:
        for name, proc in [("viewer", viewer), ("writer", writer)]:
            if proc.poll() is None:
                proc.terminate()
                try:
                    proc.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    proc.kill()
    return 0


if __name__ == "__main__":
    sys.exit(main())
