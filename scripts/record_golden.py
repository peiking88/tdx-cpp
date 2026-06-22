#!/usr/bin/env python3
"""Phase 1 黄金字节流录制工具。

连真服，hook opentdx BaseStockClient._send 抓取响应 body（解压后），
同时用 opentdx parser 解析，生成：
  tests/fixtures/golden/<name>.bin           —— 原始响应 body
  tests/fixtures/golden/<name>.expected.json —— opentdx 解析结果（C++ 须逐字段对齐）

C++ 解析 .bin 的结果须与 .expected.json 逐字段一致（浮点相对误差 < 1e-6）。

用法：
  python scripts/record_golden.py kline 600000         # 日K
  python scripts/record_golden.py transaction 600000   # 逐笔
  python scripts/record_golden.py tick 600000          # 分时
  python scripts/record_golden.py quotes 600000        # 五档
"""
import json
import os
import sys

OPENTDX = '/home/li/peiking88/opentdx'
sys.path.insert(0, OPENTDX)

FIXTURE_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                           '..', 'tests', 'fixtures', 'golden')
os.makedirs(FIXTURE_DIR, exist_ok=True)

# 测速可达的最优服务器（由 tdx server-test 选出）
BEST_IP, BEST_PORT = '119.97.185.59', 7709

# ---- hook _send 抓响应 body ----
from opentdx.client.baseStockClient import BaseStockClient  # noqa: E402

_captured = {}
_orig_send = BaseStockClient._send


def _patched_send(self, data):
    body = _orig_send(self, data)
    _captured['body'] = bytes(body)
    return body


BaseStockClient._send = _patched_send


def _save(name, body, expected):
    with open(os.path.join(FIXTURE_DIR, name + '.bin'), 'wb') as f:
        f.write(body)
    with open(os.path.join(FIXTURE_DIR, name + '.expected.json'), 'w',
              encoding='utf-8') as f:
        json.dump(expected, f, ensure_ascii=False, indent=2, default=str)
    n = len(expected) if isinstance(expected, list) else 1
    print(f'录制 {name}: {len(body)} 字节 body, {n} 条记录')


def _connect():
    from opentdx.client import QuotationClient
    client = QuotationClient()
    client.connect(BEST_IP, BEST_PORT)
    if not client.login():  # 连接后必须登录（CLAUDE.md: 连接后发 Login msg_id 0x0d）
        raise RuntimeError('opentdx login 失败')
    return client


def _market(code):
    from opentdx.const import MARKET
    return MARKET.SH if code[:1] in ('6', '5', '9') else MARKET.SZ


def record_kline(code):
    from opentdx.const import ADJUST, PERIOD
    from opentdx.parser.quotation.kline import K_Line
    client = _connect()
    parser = K_Line(_market(code), code, PERIOD.DAILY, 1, 0, 10, ADJUST.NONE)
    bars = client.call(parser)
    _save(f'kline_{code}_day', _captured['body'], bars)
    client.disconnect()


def record_transaction(code):
    from opentdx.parser.quotation.transaction import Transaction
    client = _connect()
    parser = Transaction(_market(code), code, 0, 10)
    txns = client.call(parser)
    _save(f'transaction_{code}', _captured['body'], txns)
    client.disconnect()


def record_tick(code):
    from opentdx.parser.quotation.tick_chart import TickChart
    client = _connect()
    parser = TickChart(_market(code), code, 0, 0xba00)
    ticks = client.call(parser)
    _save(f'tick_{code}', _captured['body'], ticks)
    client.disconnect()


def record_quotes(code):
    from opentdx.parser.quotation.quotes_detail import QuotesDetail
    client = _connect()
    parser = QuotesDetail([(_market(code), code)])
    quotes = client.call(parser)
    _save(f'quotes_{code}', _captured['body'], quotes)
    client.disconnect()


if __name__ == '__main__':
    if len(sys.argv) < 3:
        print(__doc__)
        sys.exit(1)
    kind, code = sys.argv[1], sys.argv[2]
    handlers = {
        'kline': record_kline,
        'transaction': record_transaction,
        'tick': record_tick,
        'quotes': record_quotes,
    }
    handler = handlers.get(kind)
    if not handler:
        print(f'未知类型: {kind}，可选: {list(handlers)}')
        sys.exit(1)
    try:
        handler(code)
    except Exception as e:
        print(f'录制失败 ({kind} {code}): {e}', file=sys.stderr)
        sys.exit(2)
