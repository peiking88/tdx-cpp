// CZSC 缠论分析器
// 直译自 ~/peiking88/czsc/crates/czsc-core/src/analyze/mod.rs:42-305
#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include "czsc/types/bi.hpp"
#include "czsc/types/enums.hpp"
#include "czsc/types/fx.hpp"
#include "czsc/types/new_bar.hpp"
#include "czsc/types/raw_bar.hpp"

namespace czsc::analyze {

class CZSC {
 public:
  size_t max_bi_num = 50;
  size_t min_bi_len = 6;
  std::vector<RawBar> bars_raw;    // 原始K线序列
  std::vector<NewBar> bars_ubi;    // 无包含关系K线序列
  std::vector<BI> bi_list;         // 已完成笔列表
  std::string symbol;
  Freq freq = Freq::kDay;

  CZSC() = default;
  // 批量初始化——逐根调用 update_bar（mod.rs:125-145）
  explicit CZSC(const std::vector<RawBar>& bars,
                size_t max_bi = 50, size_t min_bi = 6);

  // 增量更新单根K线（mod.rs:167-228）
  void update_bar(const RawBar& bar);

  // 获取分型列表（mod.rs:148-162）
  std::vector<FX> get_fx_list() const;

  // bars_ubi 中的分型（mod.rs:308-313）
  std::vector<FX> get_ubi_fxs() const;

 private:
  // 同步同 dt 延伸的 last_ubi 在 bi_list 中的镜像（mod.rs:95-123）
  void sync_extended_last_ubi_in_bis(const NewBar& last_ubi, const RawBar& bar);

  // 更新笔列表（mod.rs:230-305）
  void update_bi_();

  // 首次笔查找 / 增量笔查找 / 笔破坏检测与回滚（拆分后的三个步骤，供 update_bi_ 内部调用）
  void find_first_bi_();
  void incremental_bi_find_();
  bool last_bi_is_broken_() const;
  void handle_bi_break_();
};

}  // namespace czsc::analyze
