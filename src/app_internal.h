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

#include "browse/source.h"

namespace appdetail {

// Row geometry; must match the stylesheets (.source-row / .entry-row height
// + margin-bottom, .menu-row height) so keyboard scrolling can be computed
// without layout queries on the generated rows.
inline constexpr float kSourceRowPitch = 96.0f + 12.0f;
inline constexpr float kEntryRowPitch = 72.0f + 6.0f;

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
inline constexpr double kScreensaverSec = 180.0;  // idle time before dimming
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

// Wall-clock time 'seconds' from now as "21:45".
inline std::string ClockIn(int64_t seconds) {
  const std::time_t t = std::time(nullptr) + (std::time_t)seconds;
  std::tm tm = {};
  localtime_r(&t, &tm);
  char buf[16];
  std::strftime(buf, sizeof(buf), "%H:%M", &tm);
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

inline const char* TypeName(const browse::Entry& e) {
  switch (e.type) {
  case browse::Entry::Type::Folder: return "Folder";
  case browse::Entry::Type::Audio:  return "Audio";
  case browse::Entry::Type::Video:  return "Video";
  case browse::Entry::Type::Image:  return "Image";
  default:                          return "File";
  }
}

} // namespace appdetail

#endif
