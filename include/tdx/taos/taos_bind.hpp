// TDengine STMT 批量绑定辅助：拥有扁平数组 + TAOS_MULTI_BIND 描述符。
// header-only。注意：taos_stmt_bind_param_batch 的 TAOS_MULTI_BIND 中
// length/is_null 数组每列需要 num 个元素（每行一个），不是每列一个。
#pragma once

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

#include <taos.h>

#include "tdx/types.hpp"
#include "tdx/util/time_util.hpp"

namespace tdx::taos {

inline constexpr int kBatchSize = 1000;

// ---- K 线绑定（7 列：ts/open/high/low/close/volume/amount）----
// 全固定长度列（TIMESTAMP=8B, DOUBLE=8B）
struct KlineBindBatch {
  static constexpr int kCols = 7;
  TAOS_MULTI_BIND binds[kCols] = {};

  KlineBindBatch(const std::vector<KLine>& bars, size_t begin, size_t end) {
    size_t n = end - begin;
    int32_t n32 = static_cast<int32_t>(n);
    ts_.reserve(n);
    open_.reserve(n);
    high_.reserve(n);
    low_.reserve(n);
    close_.reserve(n);
    volume_.reserve(n);
    amount_.reserve(n);

    for (size_t i = begin; i < end; ++i) {
      const auto& b = bars[i];
      ts_.push_back(b.datetime * 1000LL);
      open_.push_back(b.open);
      high_.push_back(b.high);
      low_.push_back(b.low);
      close_.push_back(b.close);
      volume_.push_back(b.volume);
      amount_.push_back(b.amount);
    }

    // per-row length arrays: 每行 8 字节
    len8_.assign(n, 8);
    // per-row null arrays: 全非 null
    nonull_.assign(n, 0);

    BindCol(0, TSDB_DATA_TYPE_TIMESTAMP, &ts_,     8);
    BindCol(1, TSDB_DATA_TYPE_DOUBLE,    &open_,   8);
    BindCol(2, TSDB_DATA_TYPE_DOUBLE,    &high_,   8);
    BindCol(3, TSDB_DATA_TYPE_DOUBLE,    &low_,    8);
    BindCol(4, TSDB_DATA_TYPE_DOUBLE,    &close_,  8);
    BindCol(5, TSDB_DATA_TYPE_DOUBLE,    &volume_, 8);
    BindCol(6, TSDB_DATA_TYPE_DOUBLE,    &amount_, 8);
  }

 private:
  std::vector<int64_t> ts_;
  std::vector<double>  open_, high_, low_, close_, volume_, amount_;
  std::vector<int32_t> len8_;     // n 个 8（固定长度列的 per-row length）
  std::vector<char>    nonull_;   // n 个 0（per-row is_null）

  template<typename T>
  void BindCol(int idx, int type, std::vector<T>* buf, int32_t elem_size) {
    (void)elem_size;
    binds[idx].buffer_type  = type;
    binds[idx].buffer       = buf->data();
    binds[idx].buffer_length = sizeof(T);
    binds[idx].length       = len8_.data();
    binds[idx].is_null      = nonull_.data();
    binds[idx].num          = static_cast<int32_t>(buf->size());
  }
};

// ---- 复权事件绑定（7 列：ts/fenhong/peigujia/songzhuangu/peigu/category/name）----
struct AdjustBindBatch {
  static constexpr int kCols = 7;
  TAOS_MULTI_BIND binds[kCols] = {};

  explicit AdjustBindBatch(const std::vector<Xdxr>& events) {
    size_t n = events.size();
    if (n == 0) return;
    int32_t n32 = static_cast<int32_t>(n);

    ts_.reserve(n);
    fenhong_.reserve(n);
    peigujia_.reserve(n);
    songzhuangu_.reserve(n);
    peigu_.reserve(n);
    category_.reserve(n);

    size_t name_total = 0;
    for (const auto& e : events) name_total += e.name.size();
    name_buf_.reserve(name_total);
    name_len_.reserve(n);

    for (const auto& e : events) {
      int64_t epoch_s = tdx::util::date_to_epoch(
          std::stoi(e.date.substr(0, 4)),
          std::stoi(e.date.substr(5, 2)),
          std::stoi(e.date.substr(8, 2)));
      ts_.push_back(epoch_s * 1000LL);
      fenhong_.push_back(e.fenhong);
      peigujia_.push_back(e.peigujia);
      songzhuangu_.push_back(e.songzhuangu);
      peigu_.push_back(e.peigu);
      category_.push_back(e.category);

      name_buf_.insert(name_buf_.end(), e.name.begin(), e.name.end());
      name_len_.push_back(static_cast<int32_t>(e.name.size()));
    }

    len8_.assign(n, 8);
    len4_.assign(n, 4);
    nonull_.assign(n, 0);

    BindFixedCol(0, TSDB_DATA_TYPE_TIMESTAMP, &ts_,         8);
    BindFixedCol(1, TSDB_DATA_TYPE_DOUBLE,    &fenhong_,    8);
    BindFixedCol(2, TSDB_DATA_TYPE_DOUBLE,    &peigujia_,   8);
    BindFixedCol(3, TSDB_DATA_TYPE_DOUBLE,    &songzhuangu_,8);
    BindFixedCol(4, TSDB_DATA_TYPE_DOUBLE,    &peigu_,      8);
    // category: INT (4 bytes)
    binds[5].buffer_type  = TSDB_DATA_TYPE_INT;
    binds[5].buffer       = category_.data();
    binds[5].buffer_length = 4;
    binds[5].length       = len4_.data();
    binds[5].is_null      = nonull_.data();
    binds[5].num          = n32;
    // name: VARCHAR — 使用 per-row 长度数组 name_len_
    binds[6].buffer_type  = TSDB_DATA_TYPE_VARCHAR;
    binds[6].buffer       = name_buf_.data();
    binds[6].buffer_length = static_cast<uintptr_t>(name_total);
    binds[6].length       = name_len_.data();
    binds[6].is_null      = nonull_.data();
    binds[6].num          = n32;
  }

 private:
  std::vector<int64_t> ts_;
  std::vector<double>  fenhong_, peigujia_, songzhuangu_, peigu_;
  std::vector<int32_t> category_;
  std::vector<char>    name_buf_;
  std::vector<int32_t> name_len_;
  std::vector<int32_t> len8_, len4_;
  std::vector<char>    nonull_;

  template<typename T>
  void BindFixedCol(int idx, int type, std::vector<T>* buf, int32_t elem_size) {
    (void)elem_size;
    binds[idx].buffer_type  = type;
    binds[idx].buffer       = buf->data();
    binds[idx].buffer_length = sizeof(T);
    binds[idx].length       = len8_.data();
    binds[idx].is_null      = nonull_.data();
    binds[idx].num          = static_cast<int32_t>(buf->size());
  }
};

// ---- Tag 绑定辅助 ----

// kline tag: (code VARCHAR, cycle VARCHAR)
struct KlineTags {
  char code_buf[16] = {};
  char cycle_buf[16] = {};
  int32_t code_len = 0, cycle_len = 0;
  char null0 = 0, null1 = 0;
  TAOS_MULTI_BIND binds[2];

  KlineTags(const char* code, const char* period) {
    std::strncpy(code_buf, code, sizeof(code_buf) - 1);
    std::strncpy(cycle_buf, period, sizeof(cycle_buf) - 1);
    code_len = static_cast<int32_t>(std::strlen(code_buf));
    cycle_len = static_cast<int32_t>(std::strlen(cycle_buf));

    binds[0].buffer_type  = TSDB_DATA_TYPE_VARCHAR;
    binds[0].buffer       = code_buf;
    binds[0].buffer_length = sizeof(code_buf);
    binds[0].length       = &code_len;
    binds[0].is_null      = &null0;
    binds[0].num          = 1;

    binds[1].buffer_type  = TSDB_DATA_TYPE_VARCHAR;
    binds[1].buffer       = cycle_buf;
    binds[1].buffer_length = sizeof(cycle_buf);
    binds[1].length       = &cycle_len;
    binds[1].is_null      = &null1;
    binds[1].num          = 1;
  }
};

// adjust tag: (code VARCHAR)
struct AdjustTags {
  char code_buf[16] = {};
  int32_t code_len = 0;
  char null0 = 0;
  TAOS_MULTI_BIND binds[1];

  explicit AdjustTags(const char* code) {
    std::strncpy(code_buf, code, sizeof(code_buf) - 1);
    code_len = static_cast<int32_t>(std::strlen(code_buf));

    binds[0].buffer_type  = TSDB_DATA_TYPE_VARCHAR;
    binds[0].buffer       = code_buf;
    binds[0].buffer_length = sizeof(code_buf);
    binds[0].length       = &code_len;
    binds[0].is_null      = &null0;
    binds[0].num          = 1;
  }
};

}  // namespace tdx::taos
