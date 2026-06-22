#include "tdx/util/zlib_wrap.hpp"

#include <zlib.h>

namespace tdx::util {

std::vector<uint8_t> zlib_inflate(const uint8_t* data, std::size_t len) {
  if (len == 0) return {};

  z_stream zs{};
  zs.next_in = const_cast<Bytef*>(data);
  zs.avail_in = static_cast<uInt>(len);

  // inflateInit 期望标准 zlib 流（带 zlib 头）。opentdx 用 Python zlib.decompress，与此一致。
  if (inflateInit(&zs) != Z_OK) return {};

  std::vector<uint8_t> out(len * 4 + 64);
  zs.next_out = out.data();
  zs.avail_out = static_cast<uInt>(out.size());

  int rc;
  while ((rc = inflate(&zs, Z_NO_FLUSH)) != Z_STREAM_END) {
    if (rc == Z_OK || rc == Z_BUF_ERROR) {
      // 输出缓冲不足，扩容续解
      std::size_t produced = out.size() - zs.avail_out;
      std::size_t old_size = out.size();
      out.resize(old_size * 2);
      zs.next_out = out.data() + produced;
      zs.avail_out = static_cast<uInt>(out.size() - produced);
    } else {
      inflateEnd(&zs);
      return {};
    }
  }

  out.resize(out.size() - zs.avail_out);
  inflateEnd(&zs);
  return out;
}

}  // namespace tdx::util
