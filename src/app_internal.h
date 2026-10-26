// SPDX-License-Identifier: GPL-3.0-or-later
//
// Constants and helpers shared between the App translation units.
#ifndef APP_INTERNAL_H
#define APP_INTERNAL_H

#include <cstdint>
#include <cstdio>
#include <ctime>
#include <string>

#include <RmlUi/Core.h>

#include "browse/fs_source.h"
#include "browse/source.h"

namespace appdetail {

// Geometry; must match the stylesheets so keyboard scrolling can be
// computed without layout queries on the generated rows.
inline constexpr float kRailRowPitch = 80.0f + 8.0f;     // .rail-row
inline constexpr float kShelfPitch = 380.0f;              // .shelf
inline constexpr float kCardPitch = 384.0f + 24.0f;       // .card + gap
inline constexpr float kEntryRowPitch = 72.0f + 6.0f;     // list .entry-row
inline constexpr int kGridColumns = 6;                    // grid .entry-row
inline constexpr float kGridRowPitch = 252.0f + 24.0f;    // tile + gap
inline constexpr float kMenuRowPitch = 72.0f + 6.0f;      // .menu-row

// Artwork requests waiting at most; older ones are dropped.
inline constexpr size_t kArtQueueMax = 96;

// Rows kept visible above/below the selection while scrolling, so the
// next item is always in view before it is reached.
inline constexpr int kScrollMarginRows = 2;

// Rows jumped by page up/down (L1/R1, left/right in the browse list).
inline constexpr int kPageRows = 10;

inline constexpr double kToastSec = 3.0;
// A local file opens in a few milliseconds; showing a spinner for one frame
// on every launch just looks like a glitch, so the launch feedback only
// appears once the open has taken at least this long.
inline constexpr double kLaunchFeedbackSec = 0.3;
inline constexpr double kWatchInfoSec = 5.0;      // watch info bar auto-hide
inline constexpr double kScreensaverMoveSec = 20.0;
inline constexpr int kDiscoveryWaitMs = 2500;

inline constexpr int64_t kSeekSmallUs = 10 * 1000000LL;
inline constexpr int64_t kSeekLargeUs = 5 * 60 * 1000000LL;

// Seconds since startup, from RmlUi's system interface.
inline double Now() {
  Rml::SystemInterface* sys = Rml::GetSystemInterface();
  return sys ? sys->GetElapsedTime() : 0.0;
}

// Microseconds -> "1:02:33" / "12:33".
inline std::string FormatTime(int64_t us) {
  if (us < 0)
    return "--:--";
  const int64_t total = us / 1000000;
  const int h = (int)(total / 3600);
  const int m = (int)((total % 3600) / 60);
  const int s = (int)(total % 60);
  char buf[32];
  if (h > 0)
    std::snprintf(buf, sizeof(buf), "%d:%02d:%02d", h, m, s);
  else
    std::snprintf(buf, sizeof(buf), "%d:%02d", m, s);
  return buf;
}

// Bytes -> "734 KB", "1.4 GB"; "" when unknown.
inline std::string FormatSize(int64_t bytes) {
  if (bytes < 0)
    return {};
  static const char* const kUnits[] = {"B", "KB", "MB", "GB", "TB"};
  double v = (double)bytes;
  int unit = 0;
  while (v >= 1000.0 && unit < 4) { // never "1023 KB"
    v /= 1024.0;
    unit++;
  }
  char buf[32];
  if (unit == 0)
    std::snprintf(buf, sizeof(buf), "%lld B", (long long)bytes);
  else
    std::snprintf(buf, sizeof(buf), v < 10.0 ? "%.1f %s" : "%.0f %s", v, kUnits[unit]);
  return buf;
}

// Seconds since the epoch -> local "2024-03-01" (with_time: "2024-03-01
// 14:22"); "" when unknown.
inline std::string FormatDate(int64_t epoch, bool with_time) {
  if (epoch <= 0)
    return {};
  const std::time_t t = (std::time_t)epoch;
  std::tm tm = {};
  localtime_r(&t, &tm);
  char buf[32];
  std::strftime(buf, sizeof(buf), with_time ? "%Y-%m-%d %H:%M" : "%Y-%m-%d", &tm);
  return buf;
}

// A short glyph for an entry; the emoji font is loaded as a fallback face,
// so these render everywhere.
inline const char* IconFor(const browse::Entry& e) {
  switch (e.type) {
  case browse::Entry::Type::Folder: return "📁";
  case browse::Entry::Type::Audio:  return "🎵";
  case browse::Entry::Type::Video:  return "🎬";
  case browse::Entry::Type::Image:  return "🖼";
  default:                          return "📄";
  }
}

// "Video", for the info panel and cards.
inline const char* TypeLabel(const browse::Entry& e) {
  switch (e.type) {
  case browse::Entry::Type::Folder: return "Folder";
  case browse::Entry::Type::Audio:  return "Audio";
  case browse::Entry::Type::Video:  return "Video";
  case browse::Entry::Type::Image:  return "Image";
  default:                          return "File";
  }
}

// The name to show for an entry: a file's media extension ("Episode
// 1.mkv") says nothing anyone on the couch needs to know.
inline std::string DisplayName(const browse::Entry& e) {
  if (e.name.empty())
    return "(untitled)";
  const size_t dot = e.name.rfind('.');
  browse::Entry::Type type;
  if (!e.IsFolder() && dot != std::string::npos && dot > 0 &&
      browse::ClassifyFileName(e.name, type))
    return e.name.substr(0, dot);
  return e.name;
}

// Width of a resume bar ("37%") for 'position' of 'duration' seconds, or
// "0%" when there is nothing to show. Never less than a sliver, so a bar
// that is there can be seen.
inline std::string ProgressWidth(int64_t position, int64_t duration) {
  if (position <= 0 || duration <= 0)
    return "0%";
  const int64_t pct = position * 100 / duration;
  return std::to_string(pct < 2 ? 2 : pct > 100 ? 100 : pct) + "%";
}

} // namespace appdetail

#endif
