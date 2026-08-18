// ParamView 实现
#include "czsc/signals/param_view.hpp"

#include <nlohmann/json.hpp>

namespace czsc::signals {

size_t ParamView::usize(const char* key, size_t default_val) const {
  auto it = inner_->find(key);
  if (it == inner_->end()) return default_val;
  if (it->second.is_number_unsigned()) return it->second.get<size_t>();
  if (it->second.is_string()) {
    try { return std::stoul(it->second.get<std::string>()); }
    catch (const std::invalid_argument&) { return default_val; }
    catch (const std::out_of_range&) { return default_val; }
  }
  return default_val;
}

const char* ParamView::str(const char* key, const char* default_val) const {
  auto it = inner_->find(key);
  if (it == inner_->end()) return default_val;
  if (it->second.is_string()) {
    // ponytail: 返回内部引用，调用方自行 copy
    static thread_local std::string buf;
    buf = it->second.get<std::string>();
    return buf.c_str();
  }
  return default_val;
}

bool ParamView::boolean(const char* key, bool default_val) const {
  auto it = inner_->find(key);
  if (it == inner_->end()) return default_val;
  if (it->second.is_boolean()) return it->second.get<bool>();
  if (it->second.is_string()) {
    auto s = it->second.get<std::string>();
    if (s == "true" || s == "True" || s == "TRUE" || s == "1") return true;
    if (s == "false" || s == "False" || s == "FALSE" || s == "0") return false;
  }
  return default_val;
}

double ParamView::number(const char* key, double default_val) const {
  auto it = inner_->find(key);
  if (it == inner_->end()) return default_val;
  if (it->second.is_number()) return it->second.get<double>();
  if (it->second.is_string()) {
    try { return std::stod(it->second.get<std::string>()); }
    catch (const std::invalid_argument&) { return default_val; }
    catch (const std::out_of_range&) { return default_val; }
  }
  return default_val;
}

const nlohmann::json* ParamView::value(const char* key) const {
  auto it = inner_->find(key);
  return (it != inner_->end()) ? &it->second : nullptr;
}

}  // namespace czsc::signals
