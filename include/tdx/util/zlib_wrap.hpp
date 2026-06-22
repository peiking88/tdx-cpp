// zlib 解压。通达信响应压缩用 zlib（非 lzma）：zipsize != unzip_size 时需解压。
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace tdx::util {

// zlib inflate（标准 zlib 流，两级包装）。失败返回空 vector。
std::vector<uint8_t> zlib_inflate(const uint8_t* data, std::size_t len);

}  // namespace tdx::util
