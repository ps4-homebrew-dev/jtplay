// SPDX-License-Identifier: GPL-3.0-or-later
//
// The UI's icons. They are glyphs of the Lucide icon font (fonts/lucide.ttf:
// the icons below, cut from lucide-static 0.460.0, ISC licence), which is
// loaded as a fallback face: an icon is a character like any other, drawn
// in the colour and size of the text around it, and can sit inside a
// string ("<shuffle> <repeat>"). One line weight and one style throughout,
// where emoji came in as many styles as they have makers.
//
// Lucide puts its glyphs in the Private Use Area, which no text font has,
// so they never clash with what the text says.
#ifndef ICONS_H
#define ICONS_H

#include <cstring>
#include <string>

namespace icon {

inline constexpr const char* kHome = "\uE0F8";          // house
inline constexpr const char* kDrive = "\uE0F0";         // hard-drive
inline constexpr const char* kServer = "\uE157";        // server
inline constexpr const char* kPlugin = "\uE4FF";        // blocks
inline constexpr const char* kSettings = "\uE158";      // settings
inline constexpr const char* kFolder = "\uE0DC";        // folder
inline constexpr const char* kMusic = "\uE126";         // music
inline constexpr const char* kVideo = "\uE29B";         // clapperboard
inline constexpr const char* kFilm = "\uE0D4";          // film
inline constexpr const char* kImage = "\uE0F9";         // image
inline constexpr const char* kFile = "\uE0C4";          // file
inline constexpr const char* kCheck = "\uE070";         // check
inline constexpr const char* kStar = "\uE17A";          // star
inline constexpr const char* kSearch = "\uE155";        // search
inline constexpr const char* kPlay = "\uE140";          // play
inline constexpr const char* kPause = "\uE132";         // pause
inline constexpr const char* kStop = "\uE087";          // circle-stop
inline constexpr const char* kResume = "\uE085";        // circle-play
inline constexpr const char* kWarning = "\uE193";       // triangle-alert
inline constexpr const char* kGamepad = "\uE0E2";       // gamepad-2
inline constexpr const char* kShuffle = "\uE162";       // shuffle
inline constexpr const char* kRepeat = "\uE14A";        // repeat
inline constexpr const char* kRepeatOne = "\uE1FD";     // repeat-1
inline constexpr const char* kSleep = "\uE122";         // moon
inline constexpr const char* kAudio = "\uE1AB";         // volume-2
inline constexpr const char* kNext = "\uE164";          // skip-forward
inline constexpr const char* kPrevious = "\uE163";      // skip-back
inline constexpr const char* kRewind = "\uE14B";        // rewind
inline constexpr const char* kForward = "\uE0C1";       // fast-forward
inline constexpr const char* kClear = "\uE28F";         // eraser
inline constexpr const char* kTv = "\uE195";            // tv
inline constexpr const char* kRadio = "\uE146";         // radio
inline constexpr const char* kMic = "\uE11C";           // mic
inline constexpr const char* kPodcast = "\uE1FA";       // podcast
inline constexpr const char* kHeadphones = "\uE0F4";    // headphones
inline constexpr const char* kLibrary = "\uE553";       // library-big
inline constexpr const char* kChevron = "\uE073";       // chevron-right

// A source's icon as a plugin gives it, an emoji, as the icon that stands
// for the same thing; one with no counterpart is kept as it is (the emoji
// font still draws it), and none at all is a plugin's.
inline std::string FromEmoji(const std::string& emoji) {
  static const struct {
    const char* emoji;
    const char* icon;
  } kMap[] = {
    {"\U0001F4FA", kTv},         {"\U0001F4FB", kRadio},      // 📺 📻
    {"\U0001F399", kMic},        {"\U0001F3A4", kMic},        // 🎙 🎤
    {"\U0001F3A7", kHeadphones}, {"\U0001F3B5", kMusic},      // 🎧 🎵
    {"\U0001F3B6", kMusic},      {"\U0001F3AC", kVideo},      // 🎶 🎬
    {"\U0001F39E", kFilm},       {"\U0001F3A5", kFilm},       // 🎞 🎥
    {"\U0001F4F7", kImage},      {"\U0001F5BC", kImage},      // 📷 🖼
    {"\U0001F4C1", kFolder},     {"\U0001F4C2", kFolder},     // 📁 📂
    {"\U0001F5A5", kServer},     {"\U0001F4BE", kDrive},      // 🖥 💾
    {"\U0001F9E9", kPlugin},     {"\U0001F4DA", kLibrary},    // 🧩 📚
    {"\u2B50", kStar},           {"\U0001F50D", kSearch},     // ⭐ 🔍
  };
  if (emoji.empty())
    return kPlugin;
  // Variation selectors (U+FE0F) and the like after the emoji don't count.
  for (const auto& m : kMap) {
    const size_t n = std::strlen(m.emoji);
    if (emoji.compare(0, n, m.emoji) == 0 &&
        (emoji.size() == n || emoji.compare(n, 3, "\uFE0F") == 0))
      return m.icon;
  }
  return emoji;
}

} // namespace icon

#endif
