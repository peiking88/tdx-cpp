// 小端字节序读写工具。通达信协议全小端（struct '<'）。
// 运行平台（x86_64/arm64）均为小端，直接 memcpy；保留显式接口以便后续跨平台审计。
#pragma once

#include <cstdint>
#include <cstring>
#include <type_traits>

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

}  // namespace tdx::util
