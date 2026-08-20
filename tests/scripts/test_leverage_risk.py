#!/usr/bin/env python3
"""leverage-risk.py 纯函数自测：python3 tests/scripts/test_leverage_risk.py"""
import importlib.util
from pathlib import Path

_mod_path = Path(__file__).resolve().parent.parent.parent / "scripts" / "leverage-risk.py"
_spec = importlib.util.spec_from_file_location("leverage_risk", _mod_path)
_mod = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(_mod)
complete_daily_totals = _mod.complete_daily_totals


def test_incomplete_day_dropped():
    """单市场日（深交所明细未发布）必须被丢弃，否则差分出现砍半级假净卖出。"""
    rows = [
        {"ts": "2026-08-18 00:00:00", "market": "sz", "total": 100.0},
        {"ts": "2026-08-18 00:00:00", "market": "sh", "total": 200.0},
        {"ts": "2026-08-19 00:00:00", "market": "sh", "total": 210.0},  # 缺 sz
        {"ts": "2026-08-20 00:00:00", "market": "sz", "total": 105.0},
        {"ts": "2026-08-20 00:00:00", "market": "sh", "total": 215.0},
    ]
    out = complete_daily_totals(rows)
    # 8-19 被丢弃；若计入，8-19 差分 = 210-300 = -90（假暴跌），8-20 差分 = +110（假反弹）
    assert out == [("2026-08-18", 300.0), ("2026-08-20", 320.0)], out
    diffs = [out[i][1] - out[i - 1][1] for i in range(1, len(out))]
    assert diffs == [20.0], diffs


if __name__ == "__main__":
    test_incomplete_day_dropped()
    print("ok")
