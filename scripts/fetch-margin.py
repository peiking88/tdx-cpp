#!/usr/bin/env python3
"""
融资融券数据获取 — AKShare → TDengine margin 表。

仅本脚本依赖 akshare（融资余额外部数据源）；分析脚本 leverage-risk.py 零外部依赖。

用法:
  python3 scripts/fetch-margin.py                    # 上一交易日
  python3 scripts/fetch-margin.py --date 20260818     # 指定日期
  python3 scripts/fetch-margin.py --recent 30         # 近 N 日批量补录
"""
import argparse
import json
import os
import sys
import time
from datetime import datetime, timedelta
from pathlib import Path

import akshare as ak
import taosws

DB = os.environ.get("TDENGINE_DB", "tdx")
TDENGINE_URL = os.environ.get("TDENGINE_URL", "taosws://root:taosdata@localhost:6041")

HOLIDAYS = set()  # YYYYMMDD 集合，cfg/holidays.json


def load_holidays():
    p = Path(__file__).resolve().parent.parent / "cfg" / "holidays.json"
    if p.exists():
        HOLIDAYS.update(x.replace("-", "") for x in json.load(open(p)))


def get_conn():
    return taosws.connect(TDENGINE_URL)


def ensure_table(conn):
    conn.query(f"USE {DB}")
    conn.query(
        "CREATE STABLE IF NOT EXISTS margin ("
        "ts TIMESTAMP, market VARCHAR(2), code VARCHAR(8), "
        "margin_balance DOUBLE, margin_buy DOUBLE, margin_repay DOUBLE,"
        "short_balance DOUBLE, short_sell DOUBLE, short_repay DOUBLE"
        ") TAGS (secid VARCHAR(10))"
    )


def previous_trading_day(ref=None):
    d = (ref or datetime.now()) - timedelta(1)
    while d.weekday() >= 5 or d.strftime("%Y%m%d") in HOLIDAYS:
        d -= timedelta(1)
    return d


def fetch_day(conn, date_str):
    """拉取单日融资融券明细并写入 margin 表。"""
    print(f"[fetch-margin] {date_str} ...", file=sys.stderr)
    sz, sh = None, None

    # 深市
    try:
        sz = ak.stock_margin_detail_szse(date=date_str)
        time.sleep(0.5)
    except Exception as e:
        print(f"  [warn] szse detail 失败: {e}", file=sys.stderr)

    # 沪市
    try:
        sh = ak.stock_margin_detail_sse(date=date_str)
        time.sleep(0.5)
    except Exception as e:
        print(f"  [warn] sse detail 失败: {e}", file=sys.stderr)

    if sz is None and sh is None:
        print(f"  [fail] 两所数据均不可用", file=sys.stderr)
        return False

    rows = []
    ts = f"{date_str[:4]}-{date_str[4:6]}-{date_str[6:8]} 00:00:00"

    if sz is not None and len(sz) == 0:
        # 深交所对未发布日期返回空表不报错——必须告警，否则半份数据静默入库
        print(f"  [warn] szse detail 返回 0 行（明细可能未发布），{date_str} 仅沪市入库", file=sys.stderr)

    if sz is not None and len(sz) > 0:
        for _, r in sz.iterrows():
            rows.append((
                ts, "sz", str(r["证券代码"]),
                float(r.get("融资余额", 0) or 0),
                float(r.get("融资买入额", 0) or 0),
                float(r.get("融资偿还额", 0) or 0),
                float(r.get("融券余量", 0) or 0),
                float(r.get("融券卖出量", 0) or 0),
                float(r.get("融券偿还量", 0) or 0),
                f"sz{r['证券代码']}",
            ))

    if sh is not None and len(sh) > 0:
        for _, r in sh.iterrows():
            rows.append((
                ts, "sh", str(r["标的证券代码"]),
                float(r.get("融资余额", 0) or 0),
                float(r.get("融资买入额", 0) or 0),
                float(r.get("融资偿还额", 0) or 0),
                float(r.get("融券余量", 0) or 0),
                float(r.get("融券卖出量", 0) or 0),
                float(r.get("融券偿还量", 0) or 0),
                f"sh{r['标的证券代码']}",
            ))

    if not rows:
        print(f"  [warn] 无数据行", file=sys.stderr)
        return False

    # 幂等写入：按日 DELETE（超级表带时间窗口）+ INSERT，不动其他日期
    conn.query(f"DELETE FROM margin WHERE ts = '{ts}'")
    by_sec = {}
    for r in rows:
        by_sec.setdefault(r[9], []).append(r)

    for secid, rlist in by_sec.items():
        tb = f"m_{secid}"
        vals = []
        for r in rlist:
            vals.append(
                f"('{r[0]}','{r[1]}','{r[2]}',{r[3]},{r[4]},{r[5]},{r[6]},{r[7]},{r[8]})"
            )
        sql = f"INSERT INTO {tb} USING margin TAGS('{secid}') VALUES " + ",".join(vals)
        conn.query(sql)

    print(f"  [ok] {len(rows)} 行写入", file=sys.stderr)
    return True


def main():
    ap = argparse.ArgumentParser(description="融资融券数据获取 → TDengine")
    ap.add_argument("--date", help="单日 YYYYMMDD")
    ap.add_argument("--recent", type=int, help="近 N 日批量补录")
    args = ap.parse_args()

    conn = get_conn()
    ensure_table(conn)
    load_holidays()

    if args.date:
        ok = fetch_day(conn, args.date)
        sys.exit(0 if ok else 1)

    if args.recent:
        d = previous_trading_day()
        ok_count = 0
        for _ in range(args.recent * 2):
            ds = d.strftime("%Y%m%d")
            if fetch_day(conn, ds):
                ok_count += 1
                if ok_count >= args.recent:
                    break
            d = previous_trading_day(d)
        print(f"[fetch-margin] 补录 {ok_count} 日", file=sys.stderr)
        sys.exit(0)

    # 默认：上一交易日
    d = previous_trading_day()
    for _ in range(7):
        ds = d.strftime("%Y%m%d")
        if fetch_day(conn, ds):
            sys.exit(0)
        d = previous_trading_day(d)
    print("[fetch-margin] 近 7 日无数据", file=sys.stderr)
    sys.exit(1)


if __name__ == "__main__":
    main()
