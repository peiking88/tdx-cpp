// GBK 转码（系统 iconv）。通达信中文（股票名/公告）为 GBK 编码。
#pragma once

#include <cstddef>
#include <string>
#include <string_view>

namespace tdx::util {

// GBK → UTF8。转码失败时尽量返回已转部分（容错，不抛异常）。
std::string gbk_to_utf8(const char* buf, std::size_t len);
std::string gbk_to_utf8(std::string_view s);

// 去除尾部 \x00 填充。通达信固定长度字段（如 6 字节代码、16 字节名称）常用 \0 填充。
std::string trim_null(std::string_view s);

}  // namespace tdx::util
