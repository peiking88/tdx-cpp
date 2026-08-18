#!/usr/bin/env python3
"""gtest 包装器：SKIPPED 用例不得被计为 PASSED。

背景
----
``GTEST_SKIP()`` 让 gtest 二进制仍以 0 退出，ctest 据此报 ``Passed``——于是「永远跳过
的用例」（如依赖缺失的 IO / 对比回归测试）会伪装成绿。本次评审发现的历史 bug：
``test_io``（代码 002515 库中无表）与 ``test_compare_py_multi``（悬垂指针致路径解析失败）
长期 100% SKIP 却在 ctest 里显示 Passed。

做法
----
运行测试二进制并带 ``--gtest_output=json``，解析 JSON 统计 ``result == "SKIPPED"`` 的用例：
  - 发现跳过 且 ``CZSC_ALLOW_SKIP != "1"`` → 退出码 1（ctest 报 FAILED，不再伪绿）；
  - 其余情况透传二进制本身的退出码。

放行
----
导出 ``CZSC_ALLOW_SKIP=1`` 可放行跳过（用于无 TDengine / 无数据文件的开发机）。

用法
----
    gtest_no_skip.py <test_binary> [--gtest_filter=... ...]
"""
import json
import os
import subprocess
import sys
import tempfile


def collect_skipped(json_path):
    """返回 (跳过数, [用例全名 classname.name])。JSON 缺失/损坏时返回 (0, [])。"""
    try:
        with open(json_path, encoding="utf-8") as f:
            data = json.load(f)
    except Exception:
        return 0, []
    names = []
    for suite in data.get("testsuites", []):
        for tc in suite.get("testsuite", []):
            if tc.get("result") == "SKIPPED":
                names.append("{}.{}".format(tc.get("classname", suite.get("name", "")),
                                            tc.get("name", "?")))
    return len(names), names


def main():
    if len(sys.argv) < 2:
        print("usage: gtest_no_skip.py <test_binary> [gtest_args...]", file=sys.stderr)
        return 2
    bin_path = sys.argv[1]
    extra = sys.argv[2:]
    allow = os.environ.get("CZSC_ALLOW_SKIP", "0") == "1"

    fd, json_path = tempfile.mkstemp(suffix=".json", prefix="czsc_gtest_")
    os.close(fd)
    try:
        # 透传 stdout/stderr，ctest 仍能看到 gtest 原始 [  SKIPPED ] / [  PASSED  ] 输出。
        rc = subprocess.call([bin_path, f"--gtest_output=json:{json_path}"] + extra)
        count, names = collect_skipped(json_path)
    finally:
        try:
            os.unlink(json_path)
        except OSError:
            pass

    base = os.path.basename(bin_path)
    if count > 0:
        preview = ", ".join(names[:8]) + (" ..." if len(names) > 8 else "")
        if allow:
            print(f"[gtest_no_skip] {base}: {count} 个用例 SKIPPED（CZSC_ALLOW_SKIP=1 已放行）"
                  f"—— {preview}", file=sys.stderr)
        else:
            print(f"[gtest_no_skip] {base}: {count} 个用例 SKIPPED → 判失败（ctest 不计为 PASSED）"
                  f"—— {preview}。设 CZSC_ALLOW_SKIP=1 可放行。", file=sys.stderr)
            return 1
    return rc


if __name__ == "__main__":
    sys.exit(main())
