#include "tdx/proto/vipdoc_reader.hpp"

#include <cstdlib>
#include <fstream>
#include <iterator>

#include "tdx/data/scaling.hpp"
#include "tdx/util/byte_order.hpp"
#include "tdx/util/time_util.hpp"

namespace tdx::proto {
namespace {

std::vector<uint8_t> ReadFile(const std::string& path) {
  std::ifstream f(path, std::ios::binary);
  if (!f) return {};
  return std::vector<uint8_t>(std::istreambuf_iterator<char>(f),
                              std::istreambuf_iterator<char>());
}

}  // namespace

VipdocReader::VipdocReader() {
  const char* home = std::getenv("TDX_HOME");
  if (home) tdx_home_ = home;
}

VipdocReader::VipdocReader(std::string tdx_home) : tdx_home_(std::move(tdx_home)) {}

std::string VipdocReader::MarketDir(Market m) {
  switch (m) {
    case Market::SH: return "sh";
    case Market::SZ: return "sz";
    case Market::BJ: return "bj";
  }
  return "sh";
}

std::string VipdocReader::FilePath(Market m, std::string_view code,
                                   std::string_view subdir, std::string_view ext) const {
  // {home}/vipdoc/{ex}/{subdir}/{ex}{code}.{ext}
  std::string ex = MarketDir(m);
  std::string path = tdx_home_ + "/vipdoc/" + ex + "/" + std::string(subdir) + "/" +
                     ex + std::string(code) + "." + std::string(ext);
  return path;
}

std::vector<KLine> VipdocReader::ReadDay(Market market, std::string_view code) const {
  // 对齐 opentdx daily_bar_reader.py:47 <IIIIIfII>。
  auto data = ReadFile(FilePath(market, code, "lday", "day"));
  std::vector<KLine> bars;
  if (data.empty()) return bars;

  constexpr std::size_t REC = 32;
  auto s = tdx::data::GetScaling(tdx::data::ClassifySecurity(code, market), tdx::data::DataSource::Vipdoc1d);
  for (std::size_t off = 0; off + REC <= data.size(); off += REC) {
    const uint8_t* p = data.data() + off;
    uint32_t date = util::rd_u32(p);
    uint32_t open_i = util::rd_u32(p + 4);
    uint32_t high_i = util::rd_u32(p + 8);
    uint32_t low_i = util::rd_u32(p + 12);
    uint32_t close_i = util::rd_u32(p + 16);
    float amount = util::rd_f32(p + 20);
    uint32_t volume_i = util::rd_u32(p + 24);
    // reserved @28

    uint32_t year = date / 10000;
    uint32_t month = date % 10000 / 100;
    uint32_t day = date % 100;

    KLine bar;
    bar.datetime = util::date_to_epoch(year, month, day);
    bar.open = open_i * s.ohlc;
    bar.high = high_i * s.ohlc;
    bar.low = low_i * s.ohlc;
    bar.close = close_i * s.ohlc;
    bar.amount = amount * s.amount;
    bar.volume = volume_i * s.volume;
    bars.push_back(bar);
  }
  return bars;
}

std::vector<KLine> VipdocReader::ReadMin(Market market, std::string_view code,
                                         std::string_view subdir,
                                         std::string_view ext) const {
  // 对齐 opentdx lc_min_bar_reader.py <HHfffffII>。
  auto data = ReadFile(FilePath(market, code, subdir, ext));
  std::vector<KLine> bars;
  if (data.empty()) return bars;

  constexpr std::size_t REC = 32;
  for (std::size_t off = 0; off + REC <= data.size(); off += REC) {
    const uint8_t* p = data.data() + off;
    uint16_t date_h = util::rd_u16(p);
    uint16_t time_h = util::rd_u16(p + 2);
    float open = util::rd_f32(p + 4);
    float high = util::rd_f32(p + 8);
    float low = util::rd_f32(p + 12);
    float close = util::rd_f32(p + 16);
    float amount = util::rd_f32(p + 20);
    uint32_t volume = util::rd_u32(p + 24);
    // reserved @28

    // base_reader._parse_date / _parse_time
    int year = date_h / 2048 + 2004;
    int month = (date_h % 2048) / 100;
    int day = (date_h % 2048) % 100;
    int hour = time_h / 60;
    int minute = time_h % 60;

    KLine bar;
    bar.datetime = util::cst_to_epoch(year, month, day, hour, minute);
    bar.open = open;
    bar.high = high;
    bar.low = low;
    bar.close = close;
    bar.amount = amount;
    bar.volume = static_cast<double>(volume);
    bars.push_back(bar);
  }
  return bars;
}

std::vector<KLine> VipdocReader::ReadMin5(Market market, std::string_view code) const {
  return ReadMin(market, code, "fzline", "lc5");
}

std::vector<KLine> VipdocReader::ReadMin1(Market market, std::string_view code) const {
  return ReadMin(market, code, "minline", "lc1");
}

}  // namespace tdx::proto
