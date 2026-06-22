// JSON 库可用性测试：验证 nlohmann/json 能解析 0x1218 SP 响应的嵌套数组结构。
#include <gtest/gtest.h>

#include <nlohmann/json.hpp>

#include <string>
#include "tdx/util/gbk.hpp"

TEST(NlohmannJson, ParseNestedArray) {
  // 模拟 0x1218 资金流响应结构：[[today 4 元素], [five_days 6 元素]]
  auto j = nlohmann::json::parse("[[1, 2, 3, 4], [5, 6, 7, 8, 9, 10]]");
  EXPECT_EQ(j.size(), 2u);
  EXPECT_EQ(j[0].size(), 4u);
  EXPECT_EQ(j[0][0].get<int>(), 1);
  EXPECT_EQ(j[1][5].get<int>(), 10);
}

TEST(NlohmannJson, ParseDoubles) {
  auto j = nlohmann::json::parse("[[100.5, -200.3, 0.0, 50.1]]");
  EXPECT_DOUBLE_EQ(j[0][0].get<double>(), 100.5);
  EXPECT_DOUBLE_EQ(j[0][1].get<double>(), -200.3);
}

TEST(NlohmannJson, ParseAfterGbkToUtf8) {
  // 0x1218 响应是 GBK 编码 JSON（中文可能出现在字段），先 GBK→UTF8 再 parse。
  // ASCII 子集 GBK 与 UTF8 一致，这里验证流程通畅。
  std::string gbk_json = "[[100, 200]]";
  std::string utf8 = tdx::util::gbk_to_utf8(gbk_json);
  auto j = nlohmann::json::parse(utf8);
  EXPECT_EQ(j[0][1].get<int>(), 200);
}
