#include "tdx/util/gbk.hpp"

#include <iconv.h>

#include <cerrno>
#include <string>

namespace tdx::util {

std::string gbk_to_utf8(const char* buf, std::size_t len) {
  if (len == 0) return {};

  // 先尝试 GBK，失败回退 GB18030（GBK 的超集，iconv 多数发行版支持）。
  iconv_t cd = iconv_open("UTF-8", "GBK");
  if (cd == reinterpret_cast<iconv_t>(-1)) {
    cd = iconv_open("UTF-8", "GB18030");
    if (cd == reinterpret_cast<iconv_t>(-1)) {
      return std::string(buf, len);  // 无可用转码器，原样返回
    }
  }

  std::string out;
  out.resize(len * 4);  // UTF8 单字符最多 4 字节
  char* inbuf = const_cast<char*>(buf);
  std::size_t inbytes = len;
  char* outbuf = out.data();
  std::size_t outbytes = out.size();

  iconv(cd, &inbuf, &inbytes, &outbuf, &outbytes);
  iconv_close(cd);

  out.resize(out.size() - outbytes);
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
