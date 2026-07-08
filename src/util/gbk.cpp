#include "tdx/util/gbk.hpp"

#include <iconv.h>

#include <cerrno>
#include <string>

namespace tdx::util {

namespace {

// thread_local 缓存 iconv 描述符，避免高频调用重复 open/close。
// iconv 不保证线程安全，每个线程独立持有。
thread_local iconv_t g_iconv_cd = reinterpret_cast<iconv_t>(-1);

iconv_t GetIconv() {
  if (g_iconv_cd != reinterpret_cast<iconv_t>(-1)) return g_iconv_cd;
  g_iconv_cd = iconv_open("UTF-8", "GBK");
  if (g_iconv_cd == reinterpret_cast<iconv_t>(-1))
    g_iconv_cd = iconv_open("UTF-8", "GB18030");
  return g_iconv_cd;
}

}  // namespace

std::string gbk_to_utf8(const char* buf, std::size_t len) {
  if (len == 0) return {};

  iconv_t cd = GetIconv();
  if (cd == reinterpret_cast<iconv_t>(-1)) {
    return std::string(buf, len);  // 无可用转码器，原样返回
  }

  std::string out;
  out.reserve(len * 4);
  char* inbuf = const_cast<char*>(buf);
  std::size_t inbytes = len;

  // 流式转码：iconv 输出缓冲满（E2BIG）时清空继续转剩余输入，EILSEQ/EINVAL 跳 1 字节续转。
  // ponytail: E2BIG 是 iconv 流式正常信号（非错误），必须 continue——旧代码误当致命错 break，
  // 导致任何输出 >256 字节的 GBK（如 F10 全文 23KB）被截断到 ~256 字节。
  while (inbytes > 0) {
    char tmp[4096];
    char* outptr = tmp;
    std::size_t outbytes = sizeof(tmp);

    std::size_t ret = iconv(cd, &inbuf, &inbytes, &outptr, &outbytes);
    out.append(tmp, sizeof(tmp) - outbytes);

    if (ret != static_cast<std::size_t>(-1)) break;           // 全部成功
    if (errno == E2BIG) continue;                              // 输出缓冲满，继续转剩余输入
    if (errno == EILSEQ || errno == EINVAL) {
      // 跳过 1 个非法字节，重置 iconv 状态继续
      if (inbytes > 0) { ++inbuf; --inbytes; }
      iconv(cd, nullptr, nullptr, nullptr, nullptr);          // 复位 shift state
    } else {
      break;  // 其他致命错
    }
  }

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
