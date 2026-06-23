// 小端字节序读写工具。通达信协议全小端（struct '<'）。
// 运行平台（x86_64/arm64）均为小端，直接 memcpy；保留显式接口以便后续跨平台审计。
#pragma once

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string_view>
#include <type_traits>
#include <vector>

namespace tdx::util {

// 假设小端主机（x86_64/arm64 均满足）。C++17 无 std::endian，用编译器宏检测。
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__
#error "tdx-cpp 当前假设小端主机；若移植大端架构需补充字节翻转"
#endif

// 通用小端读（trivially copyable 类型）
template <typename T>
inline T read_le(const uint8_t* p) {
  static_assert(std::is_trivially_copyable_v<T>);
  T v;
  std::memcpy(&v, p, sizeof(T));
  return v;
}

// 通用小端写
template <typename T>
inline void write_le(uint8_t* p, T v) {
  static_assert(std::is_trivially_copyable_v<T>);
  std::memcpy(p, &v, sizeof(T));
}

// 常用宽度的小端读
inline uint8_t  rd_u8 (const uint8_t* p) { return *p; }
inline uint16_t rd_u16(const uint8_t* p) { return read_le<uint16_t>(p); }
inline uint32_t rd_u32(const uint8_t* p) { return read_le<uint32_t>(p); }
inline int32_t  rd_i32(const uint8_t* p) { return read_le<int32_t>(p); }
inline float    rd_f32(const uint8_t* p) { return read_le<float>(p); }

// 常用宽度的小端写
inline void wr_u16(uint8_t* p, uint16_t v) { write_le(p, v); }
inline void wr_u32(uint8_t* p, uint32_t v) { write_le(p, v); }

// vector append 变体 — 各 parser 共用，消除 4 处重复定义
// ponytail: 4× 重复 → 1× shared inline
inline void push_u8(std::vector<uint8_t>& b, uint8_t v) { b.push_back(v); }
inline void push_u16(std::vector<uint8_t>& b, uint16_t v) {
  b.push_back(static_cast<uint8_t>(v & 0xff));
  b.push_back(static_cast<uint8_t>((v >> 8) & 0xff));
}
inline void push_u32(std::vector<uint8_t>& b, uint32_t v) {
  for (int i = 0; i < 4; ++i) b.push_back(static_cast<uint8_t>((v >> (8 * i)) & 0xff));
}
inline void push_code(std::vector<uint8_t>& b, std::string_view code, std::size_t width) {
  std::size_t n = std::min<std::size_t>(code.size(), width);
  for (std::size_t i = 0; i < width; ++i)
    b.push_back(i < n ? static_cast<uint8_t>(code[i]) : 0);
}

}  // namespace tdx::util
