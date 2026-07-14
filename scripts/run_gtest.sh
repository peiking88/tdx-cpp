#!/usr/bin/env bash
# tdx-cpp gtest wrapper：把「整批用例被 GTEST_SKIP」翻译成 ctest 的 SKIPPED 状态。
#
# 背景：gtest 的 GTEST_SKIP() 不改进程退出码——SetUpTestSuite 里 SKIP 掉全部用例时，
#       进程仍退出 0，ctest 会把整个套件标为 Passed（假绿：实际零用例执行）。
#       见 tests/test_bars.cpp / test_fetch_history.cpp 的 SetUpTestSuite。
#
# 判据（基于 gtest --gtest_output=xml 的 XML，schema 见 scripts/run_gtest.sh 注释）：
#   顶层 <testsuites> 有 tests/failures（无 skipped）；子 <testsuite> 有 skipped="N"。
#
# 退出码（配合 ctest PROPERTIES SKIP_RETURN_CODE=77）：
#   1   进程非零退出（有 failure 或崩溃）/ XML 异常   → ctest FAILED（红）
#   77  无 failure 且 Σskipped == tests（全 SKIP）     → ctest SKIPPED（灰，不红不绿）
#   0   无 failure 且 有真 PASSED                      → ctest PASSED（绿）
#
# 用法：run_gtest.sh <test-binary> [gtest-args...]

set -u

if [ "$#" -lt 1 ]; then
  echo "run_gtest.sh: 缺少测试二进制参数" >&2
  exit 1
fi

bin="$1"; shift

xml="$(mktemp --suffix=.xml /tmp/tdx_gtest_XXXXXX)" || exit 1
trap 'rm -f "$xml"' EXIT

# 运行测试：透传 stdout/stderr（ctest --output-on-failure 可见），额外落 XML。
# 用环境变量而非命令行 flag：真网测试用 absl::ParseCommandLine 的 custom main，
# 命令行 --gtest_* 会被 absl 当未知 flag 拒绝；GTEST_OUTPUT/GTEST_COLOR 由 gtest
# 内部 InitGoogleTest 读取，绕开 absl flag 解析。
GTEST_OUTPUT="xml:$xml" GTEST_COLOR=no "$bin" "$@"
rc=$?

# 进程非零退出 = 有 failure（gtest rc=1）或崩溃（SIGSEGV/SIGABRT）→ 红
if [ "$rc" -ne 0 ]; then
  exit 1
fi

# rc==0 即无 failure。解析 XML 判定是否「全 SKIP」。
#   顶层 <testsuites tests="N" failures="F" ...>；子 <testsuite ... skipped="S" ...>
tests=$(grep -m1 '<testsuites' "$xml" | grep -oE 'tests="[0-9]+"' | head -1 | grep -oE '[0-9]+')
[ -z "$tests" ] && tests=0
skipped=$(grep '<testsuite ' "$xml" | grep -oE 'skipped="[0-9]+"' | grep -oE '[0-9]+' \
          | awk '{s+=$1} END{print s+0}')

# XML 解析失败（空/异常）→ 暴露为失败，不静默转绿
if [ "$tests" -eq 0 ]; then
  echo "run_gtest.sh: XML 未解析到用例（$xml），疑似异常" >&2
  exit 1
fi

# 全 SKIP → 灰
if [ "$skipped" -eq "$tests" ]; then
  exit 77
fi

# 有真 PASSED → 绿
exit 0
