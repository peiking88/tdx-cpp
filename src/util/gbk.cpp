#include "tdx/util/gbk.hpp"

#include <iconv.h>

#include <cerrno>
#include <string>

namespace tdx::util {

std::string gbk_to_utf8(const char* buf, std::size_t len) {
  if (len == 0) return {};

  iconv_t cd = iconv_open("UTF-8", "GBK");
  if (cd == reinterpret_cast<iconv_t>(-1)) {
    cd = iconv_open("UTF-8", "GB18030");
    if (cd == reinterpret_cast<iconv_t>(-1)) {
      return std::string(buf, len);  // 无可用转码器，原样返回
    }
  }

  std::string out;
  out.reserve(len * 4);
  char* inbuf = const_cast<char*>(buf);
  std::size_t inbytes = len;

  // 循环转码：遇非法序列时跳过 1 字节续转，避免截断（iconv 默认遇错即停）。
  while (inbytes > 0) {
    char tmp[256];
    char* outptr = tmp;
    std::size_t outbytes = sizeof(tmp);

    std::size_t ret = iconv(cd, &inbuf, &inbytes, &outptr, &outbytes);
    out.append(tmp, sizeof(tmp) - outbytes);

    if (ret != static_cast<std::size_t>(-1)) break;           // 全部成功
    if (errno == EILSEQ || errno == EINVAL) {
      // 跳过 1 个非法字节，重置 iconv 状态继续
      if (inbytes > 0) { ++inbuf; --inbytes; }
      iconv(cd, nullptr, nullptr, nullptr, nullptr);          // 复位 shift state
    } else {
      break;  // E2BIG 等——不应在 256B 缓冲发生，视为致命错
    }
  }

  iconv_close(cd);
  return out;
}

std::string gbk_to_utf8(std::string_view s) {
  return gbk_to_utf8(s.data(), s.size());
}

std::string trim_null(std::string_view s) {
  std::size_t n = s.find('\0');
  if (n == std::string_view::npos) return std::string(s);
  return std::string(s.substr(0, n));
}

}  // namespace tdx::util
