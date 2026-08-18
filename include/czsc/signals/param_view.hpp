// ParamView：统一参数只读视图
// 直译自 ~/peiking88/czsc/crates/czsc-signals/src/params.rs:1-63
#pragma once

#include <nlohmann/json.hpp>
#include <cstddef>
#include <string>
#include <unordered_map>

namespace czsc::signals {

class ParamView {
 public:
  explicit ParamView(const std::unordered_map<std::string, nlohmann::json>& inner)
      : inner_(&inner) {}

  // 读取 usize 参数
  size_t usize(const char* key, size_t default_val) const;

  // 读取字符串参数
  const char* str(const char* key, const char* default_val) const;

  // 读取 bool 参数
  bool boolean(const char* key, bool default_val) const;

  // 读取 double 参数
  double number(const char* key, double default_val) const;

  // 原始访问
  const nlohmann::json* value(const char* key) const;

 private:
  const std::unordered_map<std::string, nlohmann::json>* inner_;
};

}  // namespace czsc::signals
