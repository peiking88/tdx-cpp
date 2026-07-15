// import 标的过滤单元测试：IsAStock / IsKronosTarget 代码段判定。
// 纯头文件函数（无外部依赖），离线可跑。
#include <gtest/gtest.h>

#include "tdx/taos/taos_import.hpp"

using tdx::taos::IsAStock;
using tdx::taos::IsKronosTarget;

// 个股应被两者保留
TEST(ImportFilter, IndividualStocks) {
  EXPECT_TRUE(IsAStock("000001"));  // 深主板（平安银行）
  EXPECT_TRUE(IsAStock("300001"));  // 创业板
  EXPECT_TRUE(IsAStock("600000"));  // 沪主板
  EXPECT_TRUE(IsAStock("688000"));  // 科创板
  EXPECT_TRUE(IsAStock("430047"));  // 北交所
  EXPECT_TRUE(IsAStock("830000"));  // 北交所（非 88 板块）

  EXPECT_TRUE(IsKronosTarget("000001"));
  EXPECT_TRUE(IsKronosTarget("300001"));
  EXPECT_TRUE(IsKronosTarget("600000"));
  EXPECT_TRUE(IsKronosTarget("688000"));
  EXPECT_TRUE(IsKronosTarget("430047"));
  EXPECT_TRUE(IsKronosTarget("830000"));
}

// 大盘指数：IsAStock 保留，IsKronosTarget 保留（--kronos 关键语义）
TEST(ImportFilter, MarketIndices) {
  EXPECT_TRUE(IsAStock("000001"));  // 上证指数（SH，按 0x 分支保留）
  EXPECT_TRUE(IsAStock("399001"));  // 深证成指
  EXPECT_TRUE(IsAStock("399006"));  // 创业板指
  EXPECT_TRUE(IsAStock("999999"));  // 上证指数（99xxxx 沪市指数）
  EXPECT_TRUE(IsAStock("880001"));  // 板块指数（IsAStock 保留）

  EXPECT_TRUE(IsKronosTarget("000001"));
  EXPECT_TRUE(IsKronosTarget("399001"));
  EXPECT_TRUE(IsKronosTarget("399006"));
  EXPECT_TRUE(IsKronosTarget("999999"));
}

// ETF/LOF/板块指数：IsAStock 保留，IsKronosTarget 排除（--kronos 核心排除项）
TEST(ImportFilter, KronosExcludesFundsAndSectors) {
  // ETF/LOF：IsAStock 保留
  EXPECT_TRUE(IsAStock("510050"));  // 5xxxxx ETF
  EXPECT_TRUE(IsAStock("159001"));  // 159xxx 深市 ETF
  EXPECT_TRUE(IsAStock("160106"));  // 16xxxx 深市 LOF
  // 板块指数：IsAStock 保留
  EXPECT_TRUE(IsAStock("880001"));  // 88xxxx 板块指数

  // --kronos：ETF/LOF/板块全部排除
  EXPECT_FALSE(IsKronosTarget("510050"));
  EXPECT_FALSE(IsKronosTarget("159001"));
  EXPECT_FALSE(IsKronosTarget("160106"));
  EXPECT_FALSE(IsKronosTarget("880001"));
}

// 债券/B 股/港股通：两者均排除
TEST(ImportFilter, ExcludedByBoth) {
  EXPECT_FALSE(IsAStock("110000"));  // 1xxxxx 债券（非 159/16）
  EXPECT_FALSE(IsAStock("200001"));  // 2xxxxx B 股
  EXPECT_FALSE(IsAStock("700001"));  // 7xxxxx 港股通
  EXPECT_FALSE(IsAStock("900001"));  // 900xxx B 股（非 99 指数）

  EXPECT_FALSE(IsKronosTarget("110000"));
  EXPECT_FALSE(IsKronosTarget("200001"));
  EXPECT_FALSE(IsKronosTarget("700001"));
  EXPECT_FALSE(IsKronosTarget("900001"));
}
