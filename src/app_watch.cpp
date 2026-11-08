// SPDX-License-Identifier: GPL-3.0-or-later
//
// Playback. It starts full screen: for video the UI chrome hides and the
// decoded frames fill the screen behind a transparent document body, with
// an auto-hiding info/transport bar; for audio a persistent now-playing
// card is shown instead. The options button opens a playback menu (tracks,
// subtitles, speed) over either.
//
// Back leaves full screen without stopping, as in Kodi: the menus come
// back over the video, on a see-through background, and can be used as
// ever while it plays on and moves through its queue. Triangle (Tab), or
// selecting the item that plays, goes back to full screen.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <random>

#include "app.h"
#include "app_internal.h"
#include "gamepad.h"
#include "icons.h"

using namespace appdetail;

namespace {

constexpr double kSpeeds[] = {0.5, 0.75, 1.0, 1.25, 1.5, 2.0};
constexpr int kSpeedCount = (int)(sizeof(kSpeeds) / sizeof(kSpeeds[0]));

// Up next: the card shows over the last this many seconds of a video long
// enough to be an episode.
constexpr int64_t kUpNextSec = 15;
constexpr int64_t kUpNextMinDurationSec = 5 * 60;

// Sleep timer choices, minutes; -1 = at the end of what is playing.
constexpr int kSleepChoices[] = {0, 15, 30, 45, 60, 90, 120, -1};
constexpr int kSleepChoiceCount = (int)(sizeof(kSleepChoices) / sizeof(kSleepChoices[0]));

// Seek steps grow while the same direction is pressed (or held) in quick
// succession: three presses of each, then the largest step from there on.
constexpr int64_t kSeekSteps[] = {10, 10, 10, 30, 30, 30, 60, 60, 120, 300};
constexpr int kSeekStepCount = (int)(sizeof(kSeekSteps) / sizeof(kSeekSteps[0]));

const char* RepeatName(int repeat) {
  static const char* const kNames[] = {"Off", "All", "One"};
  return kNames[repeat];
}

const char* FitName(Player::Fit fit) {
  switch (fit) {
  case Player::Fit::Fill:    return "Fill (crop)";
  case Player::Fit::Stretch: return "Stretch";
  default:                   return "Fit";
  }
}

// "+0.15 s", "0 s".
std::string DelayLabel(double seconds) {
  if (std::fabs(seconds) < 0.0005)
    return "0 s";
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%+.2f s", seconds);
  return buf;
}

std::string SpeedLabel(double speed) {
  char buf[16];
  std::snprintf(buf, sizeof(buf), "%g×", speed);
  return buf;
}

// When what is playing will be over, at this speed, by the wall clock:
// "Ends at 21:40". Only for something long enough to plan an evening by.
std::string EndsAt(int64_t pos, int64_t dur, double speed) {
  if (pos < 0 || dur < 5 * 60 * 1000000LL || pos >= dur || speed <= 0.0)
    return {};
  const std::time_t end =
    std::time(nullptr) + (std::time_t)((double)(dur - pos) / 1e6 / speed);
  std::tm tm = {};
  localtime_r(&end, &tm);
  char buf[32];
  std::strftime(buf, sizeof(buf), "Ends at %H:%M", &tm);
  return buf;
}

// Percentage of 'part' in 'whole' as a CSS width, two decimals so the bar
// moves smoothly even on a three-hour film.
std::string Percent(int64_t part, int64_t whole) {
  if (whole <= 0 || part <= 0)
    return "0%";
  const double pct = std::clamp((double)part * 100.0 / (double)whole, 0.0, 100.0);
  char buf[16];
  std::snprintf(buf, sizeof(buf), "%.2f%%", pct);
  return buf;
}

// Index of the next track after the one with id 'current' in 'direction',
// wrapping around; 'current' not found counts as before the first.
template <typename T>
size_t NextTrack(const std::vector<T>& tracks, int current, int direction) {
  size_t i = 0;
  while (i < tracks.size() && tracks[i].stream_index != current)
    i++;
  if (i == tracks.size())
    return direction > 0 ? 0 : tracks.size() - 1;
  return (i + tracks.size() + (direction > 0 ? 1 : -1)) % tracks.size();
}

} // namespace

// Plays an item, offering to pick up where it was left off. The question
// is a small dialog in the middle of the screen, Kodi style.
void App::PlayEntry(const browse::Entry& entry) {
  // The item that plays behind the menus: back to it rather than start it
  // over.
  if (bind_watching_ && !bind_fullscreen_ && !launch_request_) {
    library::Ref ref;
    const bool same = playing_identified_
      ? Identify(entry, ref) && ref.source_key == playing_ref_.source_key &&
        ref.entry.id == playing_ref_.entry.id
      : entry.id == playing_.id && current_source_ == queue_.source;
    if (same) {
      SetFullscreen(true);
      return;
    }
  }
  if (entry.IsImage()) {
    int index = 0;
    const browse::Listing photos = FolderPhotos(entry, index);
    OpenPhotos(photos, index, false);
    return;
  }
  library::Ref ref;
  library::Progress p;
  if (!entry.IsFolder() && Identify(entry, ref) &&
      library_.GetProgress(ref.source_key, ref.entry.id, p) &&
      !p.watched && p.position > 0) {
    const int64_t at = p.position;
    menu_items_.clear();
    menu_items_.push_back({"Resume from " + FormatTime(at * 1000000), nullptr,
      [this, entry, at](int) { Launch(entry, at); }});
    menu_items_.push_back({"Play from beginning", nullptr,
      [this, entry](int) { Launch(entry, 0); }});
    Set("menu_title", bind_menu_title_, DisplayName(entry));
    // Where it was left, of how long, when the length is known.
    if (p.duration > at) {
      Set("menu_meta", bind_menu_meta_,
          Rml::String(FormatTime(at * 1000000) + " of " + FormatTime(p.duration * 1000000) +
                      "  ·  " + FormatTime((p.duration - at) * 1000000) + " left"));
      Set("menu_progress", bind_menu_progress_, Percent(at, p.duration));
    } else {
      Set("menu_meta", bind_menu_meta_, Rml::String());
    }
    Set("menu_dialog", bind_menu_dialog_, true);
    sel_menu_ = 0;
    RebuildMenuRows();
    Set("menu_open", bind_menu_open_, true);
    return;
  }
  Launch(entry, 0);
}

// Starts an item. Nothing about the UI changes here: the browse list stays
// up -- and with it the topbar busy indicator -- until the worker reports
// that the player is running. Entering watch mode any earlier hides the
// shell, and with it the only sign of life, for as long as the resolve and
// the open take, which on a remote source is most of the wait.
void App::Launch(const browse::Entry& entry, int64_t start_seconds, bool from_queue) {
  CancelLaunch();
  // Whatever is playing now is about to be replaced: remember how far it
  // got while the player still knows.
  if (bind_watching_)
    RecordProgress(true);

  launch_entry_ = entry;
  launch_direction_ = 0; // PlayNeighbor() sets it after this returns
  launch_request_ = ++launch_seq_;
  launch_phase_ = kLaunchResolving;
  launch_visible_at_ = Now() + kLaunchFeedbackSec;
  launch_from_queue_ = from_queue;
  // Moving on through the queue behind the menus stays behind them;
  // anything chosen from the menus starts full screen.
  launch_behind_ = from_queue && bind_watching_ && !bind_fullscreen_;

  // Chosen from the menus: its folder, as listed, becomes the play queue
  // once it plays (an item from elsewhere, a card at home, is a queue of
  // its own).
  const browse::SourcePtr source = from_queue ? queue_.source : current_source_;
  if (!from_queue) {
    launch_queue_ = PlayQueue{current_source_, std::string(), browse::Listing{entry}};
    if (view_ == View::Browse && !path_.empty()) {
      const BrowseLevel& level = path_.back();
      for (const browse::Entry& e : level.entries) {
        if (e.id == entry.id) {
          launch_queue_ = PlayQueue{current_source_, level.id, level.entries};
          break;
        }
      }
    }
  }

  library::Ref ref;
  const bool identified = IdentifyIn(entry, source, ref);

  // Resolve and open on the worker: a source with expiring URLs mints one
  // here, as late as possible, and both steps may block on the network.
  const browse::Entry item = entry;
  const uint32_t request = launch_request_;
  busy_ops_++;
  PostTask([this, item, source, request, start_seconds, ref, identified] {
    std::string error;
    std::string uri = item.uri;
    bool ok;
    if (source) {
      launch_phase_ = kLaunchResolving;
      ok = source->Resolve(item, uri, error);
    } else {
      ok = !uri.empty();
    }
    if (ok) {
      launch_phase_ = kLaunchOpening;
      ok = player_->Open(uri, error, start_seconds);
    } else if (error.empty()) {
      error = "this item has nothing to play";
    }

    Reply([this, request, ok, error, ref, identified, start_seconds] {
      if (!launch_request_ || request != launch_request_) {
        // Abandoned while it was opening (back, or another item started).
        // If it did open, take it down again rather than showing it.
        if (ok)
          PostTask([this] { player_->Stop(); });
        // Cancelled outright, from the menus over something playing, after
        // the player had let that go to open this one: nothing plays now.
        if (!launch_request_ && bind_watching_ && (ok || !player_->IsPlaying()))
          StopPlayback();
      } else if (!ok) {
        ShowToast(error.empty() ? "Playback failed" : error, icon::kWarning);
        const int direction = launch_direction_;
        const bool from_queue = launch_from_queue_;
        const browse::Entry failed = launch_entry_;
        CancelLaunch();
        // Chosen from the menus over something playing, and it never got
        // as far as the player (which stops what it plays only to open the
        // next): what was playing plays on.
        if (bind_watching_ && !from_queue && player_->IsPlaying())
          return;
        // Moving through a folder from the player: whatever was playing has
        // been stopped for this one, so carry on past it (as a playlist
        // skips a broken entry) or leave the player when the folder ends.
        if (bind_watching_) {
          playing_ = failed;
          // Shuffle and repeat-all never run out of neighbours; a folder
          // that is all broken files must still come to an end.
          const bool gave_up = ++launch_failures_ >= (int)std::max<size_t>(1, PlayOrder().size());
          if (direction == 0 || gave_up || !PlayNeighbor(direction)) {
            launch_failures_ = 0;
            StopPlayback();
          }
        }
      } else {
        // The player is running: only now is there anything to look at.
        playing_ = launch_entry_;
        launch_failures_ = 0;
        playing_ref_ = ref;
        playing_identified_ = identified;
        progress_saved_at_ = Now();
        watch_last_pos_us_ = watch_last_dur_us_ = -1;
        watch_last_seekable_ = false;
        if (!launch_from_queue_)
          queue_ = std::move(launch_queue_);
        const bool behind = launch_behind_;
        CancelLaunch();
        EnterWatch(playing_, !behind);
        if (start_seconds > 0)
          ShowToast("Resumed at " + FormatTime(start_seconds * 1000000), icon::kResume);
      }
    });
    busy_ops_--;
  });
}

// Cancelling stops us caring about the result, and asks the player to give
// up on an open in progress. If the open got through anyway, Update() throws
// the result away and stops the stream again.
void App::CancelLaunch() {
  if (!launch_request_)
    return;
  // Frees the worker from a slow open rather than letting it run on.
  player_->AbortOpen();
  launch_request_ = 0;
  launch_phase_ = kLaunchIdle;
  launch_from_queue_ = false;
  launch_behind_ = false;
  Set("launch_status", bind_launch_status_, Rml::String());
}

void App::CancelForegroundLaunch() {
  if (launch_request_ && !launch_behind_)
    CancelLaunch();
}

void App::EnterWatch(const browse::Entry& entry, bool fullscreen) {
  visualizer_.Reset();
  Set("watching", bind_watching_, true);
  Set("fullscreen", bind_fullscreen_, fullscreen);
  Set("watch_audio", bind_watch_audio_, entry.IsAudio());
  Set("video_behind", bind_video_behind_, !fullscreen && !entry.IsAudio());
  back_held_ = false;
  Set("watch_name", bind_watch_name_, DisplayName(entry));
  Set("watch_description", bind_watch_description_, entry.description);
  Set("watch_paused", bind_watch_paused_, false);
  Set("watch_pos", bind_watch_pos_, Rml::String());
  Set("watch_dur", bind_watch_dur_, Rml::String());
  Set("watch_progress", bind_watch_progress_, Rml::String("0%"));
  Set("watch_buffered", bind_watch_buffered_, Rml::String("0%"));
  Set("watch_speed", bind_watch_speed_, Rml::String());
  Set("watch_vtrack", bind_watch_vtrack_, Rml::String());
  Set("watch_atrack", bind_watch_atrack_, Rml::String());
  Set("watch_chapter", bind_watch_chapter_, Rml::String());
  Set("upnext", bind_upnext_, false);
  upnext_dismissed_ = false;
  // A seek run belongs to the item it was made in, not the next one.
  Set("watch_seek", bind_watch_seek_, Rml::String());
  seek_accum_us_ = 0;
  seek_run_ = 0;
  seek_bubble_until_ = 0.0;
  if (!chapter_marks_.empty()) {
    chapter_marks_.clear();
    model_.DirtyVariable("chapter_marks");
  }
  RefreshImageBindings();
  if (fullscreen)
    ShowWatchInfo(kWatchInfoSec);
  else
    Set("info_visible", bind_info_visible_, false);
  // Home's row of what is playing: there, or now this.
  if (view_ == View::Home)
    RebuildShelves();
  if (view_ == View::Browse)
    RebuildDetail(); // its status: now playing
}

void App::ExitWatch() {
  const bool was_fullscreen = bind_fullscreen_;
  Set("watching", bind_watching_, false);
  Set("fullscreen", bind_fullscreen_, false);
  Set("video_behind", bind_video_behind_, false);
  back_held_ = false;
  Set("upnext", bind_upnext_, false);
  Set("info_visible", bind_info_visible_, false);
  // The playback menu goes with the player; one over the menus stays.
  if (bind_menu_open_ && was_fullscreen)
    CloseMenu();
  playing_identified_ = false;
  RefreshImageBindings();
  OnLibraryChanged(); // progress marks, Continue watching
}

void App::SetFullscreen(bool on) {
  if (!bind_watching_ || bind_fullscreen_ == on)
    return;
  back_held_ = false;
  if (bind_menu_open_)
    CloseMenu(); // the playback menu, or the menu over the menus
  Set("fullscreen", bind_fullscreen_, on);
  Set("video_behind", bind_video_behind_, !on && !bind_watch_audio_);
  if (on) {
    ShowWatchInfo(kWatchInfoSec);
  } else {
    Set("upnext", bind_upnext_, false);
    Set("info_visible", bind_info_visible_, false);
    Set("watch_seek", bind_watch_seek_, Rml::String());
    // The lists come back showing how far it has got.
    RecordProgress(false);
    progress_saved_at_ = Now();
    // The shell was hidden: put its lists back where they were.
    scroll_entries_pending_ = true;
    scroll_sources_pending_ = true;
    if (view_ == View::Browse)
      RebuildDetail(); // its status: now playing
  }
  // Home's row of what is playing (it was out of sight: it may be new).
  if (view_ == View::Home)
    RebuildShelves();
  RefreshImageBindings();
}

void App::StopPlayback() {
  CancelLaunch();
  if (bind_watching_)
    RecordProgress(true);
  ExitWatch();
  PostTask([this] { player_->Stop(); });
}

void App::ShowWatchInfo(double seconds) {
  info_deadline_ = Now() + seconds;
  Set("info_visible", bind_info_visible_, true);
}

void App::UpdateWatchOverlay() {
  const int64_t pos = player_->PositionUs();
  const int64_t dur = player_->DurationUs();
  const double speed = player_->Speed();
  if (pos >= 0 && player_->IsPlaying()) {
    watch_last_pos_us_ = pos;
    watch_last_dur_us_ = dur;
    watch_last_seekable_ = player_->CanSeek();
  }

  // Only worth drawing while it runs ahead of the position, i.e. for a
  // network stream that is still filling its cache.
  int64_t buffered = player_->BufferedUs();
  if (buffered <= pos || dur <= 0)
    buffered = 0;

  Set("watch_pos", bind_watch_pos_, pos >= 0 ? FormatTime(pos) : Rml::String());
  Set("watch_dur", bind_watch_dur_, dur > 0 ? FormatTime(dur) : Rml::String());
  Set("watch_ends", bind_watch_ends_,
      player_->CanSeek() && player_->DurationReliable() ? EndsAt(pos, dur, speed)
                                                         : Rml::String());
  Set("watch_progress", bind_watch_progress_, Percent(pos, dur));
  if (!bind_watch_seek_.empty() && dur > 0) {
    // Half the bubble's width (100px) as a share of the 1920px bar.
    const double half = 100.0 * 100.0 / 1920.0;
    const double at = std::clamp((double)std::max<int64_t>(pos, 0) * 100.0 / (double)dur,
                                 half, 100.0 - half);
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%.2f%%", at);
    Set("watch_seek_at", bind_watch_seek_at_, Rml::String(buf));
  }
  Set("watch_buffered", bind_watch_buffered_, Percent(buffered, dur));
  Set("watch_speed", bind_watch_speed_,
      std::fabs(speed - 1.0) < 0.01 ? Rml::String() : SpeedLabel(speed));
  Set("watch_paused", bind_watch_paused_, player_->IsPaused());
  Set("watch_seekable", bind_watch_seekable_, player_->CanSeek());

  // The labels of the streams actually being decoded, so they follow a
  // track switch.
  Set("watch_vtrack", bind_watch_vtrack_, player_->CurrentVideoLabel());
  Set("watch_atrack", bind_watch_atrack_, player_->CurrentAudioLabel());

  // What plays after this one, for the "Up next" line.
  Rml::String next;
  if ((bind_watch_audio_ || autoplay_) && repeat_ != Repeat::One) {
    const int i = NeighborIndex(+1);
    if (i >= 0)
      next = DisplayName(queue_.entries[i]);
  }
  Set("watch_next", bind_watch_next_, next);

  // Chapters: ticks on the timeline, and where we are among them.
  const std::vector<ChapterInfo> chapters = player_->Chapters();
  std::vector<MarkRow> marks;
  if (dur > 0 && chapters.size() > 1) {
    for (size_t i = 1; i < chapters.size(); i++) // the first starts at 0
      marks.push_back({Percent(chapters[i].start_us, dur)});
  }
  if (marks != chapter_marks_) {
    chapter_marks_ = std::move(marks);
    model_.DirtyVariable("chapter_marks");
  }
  Rml::String chapter;
  const int ci = player_->CurrentChapter();
  if (chapters.size() > 1 && ci >= 0 && ci < (int)chapters.size()) {
    chapter = "Chapter " + std::to_string(ci + 1) + "/" + std::to_string(chapters.size());
    if (!chapters[ci].title.empty())
      chapter += "  ·  " + chapters[ci].title;
  }
  Set("watch_chapter", bind_watch_chapter_, chapter);

  Rml::String modes;
  if (shuffle_)
    modes += icon::kShuffle;
  if (repeat_ == Repeat::All)
    modes += modes.empty() ? std::string(icon::kRepeat) : std::string(" ") + icon::kRepeat;
  else if (repeat_ == Repeat::One)
    modes += modes.empty() ? std::string(icon::kRepeatOne) : std::string(" ") + icon::kRepeatOne;
  if (sleep_at_ > 0 || sleep_end_of_item_)
    modes += modes.empty() ? std::string(icon::kSleep) : std::string(" ") + icon::kSleep;
  Set("watch_modes", bind_watch_modes_, modes);

  UpdateUpNext(pos, dur);
}

// The last seconds of an episode: a card with what comes next and a
// countdown, Netflix style. Select plays it at once, back dismisses it (and
// then the player stops at the end rather than going on).
void App::UpdateUpNext(int64_t pos, int64_t dur) {
  bool show = false;
  if (bind_fullscreen_ && !bind_watch_audio_ && autoplay_ && !upnext_dismissed_ &&
      repeat_ != Repeat::One &&
      !sleep_end_of_item_ && !launch_request_ && player_->CanSeek() &&
      player_->DurationReliable() && pos >= 0 && dur >= kUpNextMinDurationSec * 1000000 &&
      dur - pos <= kUpNextSec * 1000000) {
    const int i = NeighborIndex(+1);
    if (i >= 0) {
      const browse::Entry& next = queue_.entries[i];
      show = true;
      Set("upnext_name", bind_upnext_name_, DisplayName(next));
      const double speed = std::max(0.1, player_->Speed());
      const int left = (int)std::ceil((double)(dur - pos) / 1e6 / speed);
      Set("upnext_count", bind_upnext_count_,
          Rml::String("Playing in " + std::to_string(std::max(0, left))));
      const std::string uri = ArtUri(next);
      const std::string path = uri.empty() ? std::string() : ImagePathFor(uri, true);
      Set("upnext_art", bind_upnext_art_, CoverDecorator(path));
    }
  }
  Set("upnext", bind_upnext_, show);
}

void App::UpdateSleepTimer(double now) {
  if (sleep_at_ <= 0 || now < sleep_at_)
    return;
  sleep_at_ = 0;
  if (bind_watching_ || launch_request_) {
    StopPlayback();
    ShowToast("Sleep timer: playback stopped", icon::kSleep);
  }
  if (bind_photo_)
    ClosePhotos();
}

std::string App::SleepLabel() const {
  if (sleep_end_of_item_)
    return "End of this item";
  if (sleep_at_ <= 0)
    return "Off";
  const int minutes = (int)std::ceil((sleep_at_ - Now()) / 60.0);
  return std::to_string(std::max(1, minutes)) + " min left";
}

// T (triangle) / the playback menu: next audio track, wrapping around.
void App::CycleAudioTrack(int direction) {
  const std::vector<AudioTrackInfo> tracks = player_->AudioTracks();
  if (tracks.size() < 2) {
    if (!tracks.empty())
      ShowToast("Only one audio track");
    return;
  }
  const size_t next = NextTrack(tracks, player_->CurrentAudioStream(), direction);
  if (player_->SelectAudioTrack(tracks[next].stream_index)) {
    ShowToast("Audio " + std::to_string(next + 1) + "/" +
              std::to_string(tracks.size()) + ": " + tracks[next].label, icon::kAudio);
  }
}

// Subtitles cycle through "off" and every track.
void App::CycleSubtitleTrack(int direction) {
  std::vector<SubtitleTrackInfo> choices = player_->SubtitleTracks();
  if (choices.empty())
    return;
  SubtitleTrackInfo off;
  off.stream_index = -1;
  off.label = "Off";
  choices.insert(choices.begin(), off);
  const size_t next = NextTrack(choices, player_->CurrentSubtitleStream(), direction);
  player_->SelectSubtitleTrack(choices[next].stream_index);
}

void App::OpenWatchMenu() {
  menu_items_.clear();
  Set("menu_title", bind_menu_title_, Rml::String("Playback"));
  Set("menu_dialog", bind_menu_dialog_, false);

  if (player_->AudioTracks().size() > 1) {
    menu_items_.push_back({"Audio",
      [this] { return player_->CurrentAudioLabel(); },
      [this](int d) { CycleAudioTrack(d < 0 ? -1 : +1); }});
  }
  if (!player_->SubtitleTracks().empty()) {
    menu_items_.push_back({"Subtitles",
      [this] {
        const int id = player_->CurrentSubtitleStream();
        for (const SubtitleTrackInfo& t : player_->SubtitleTracks())
          if (t.stream_index == id)
            return t.label;
        return std::string("Off");
      },
      [this](int d) { CycleSubtitleTrack(d < 0 ? -1 : +1); }});
  }
  if (player_->Chapters().size() > 1) {
    menu_items_.push_back({"Chapter",
      [this] {
        const std::vector<ChapterInfo> chapters = player_->Chapters();
        const int i = player_->CurrentChapter();
        if (i < 0 || i >= (int)chapters.size())
          return std::string("-");
        std::string label = std::to_string(i + 1) + "/" + std::to_string(chapters.size());
        if (!chapters[i].title.empty())
          label += " " + chapters[i].title;
        return label.size() > 28 ? label.substr(0, 27) + "…" : label;
      },
      [this](int d) {
        const int n = (int)player_->Chapters().size();
        const int i = std::max(0, player_->CurrentChapter());
        player_->SeekChapter(std::clamp(i + (d < 0 ? -1 : 1), 0, n - 1));
      }});
  }
  if (player_->CanSeek()) {
    menu_items_.push_back({"Speed",
      [this] { return SpeedLabel(player_->Speed()); },
      [this](int d) {
        int i = 0;
        while (i < kSpeedCount - 1 && kSpeeds[i] < player_->Speed() - 0.01)
          i++;
        i = d == 0 ? (i + 1) % kSpeedCount
                   : std::clamp(i + (d < 0 ? -1 : 1), 0, kSpeedCount - 1);
        player_->SetSpeed(kSpeeds[i]);
      }});
  }
  if (PlayOrder().size() > 1) {
    menu_items_.push_back({"Shuffle",
      [this] { return std::string(shuffle_ ? "On" : "Off"); },
      [this](int) { SetShuffle(!shuffle_); }});
  }
  menu_items_.push_back({"Repeat",
    [this] { return std::string(RepeatName((int)repeat_)); },
    [this](int d) {
      const int n = (int)Repeat::Count;
      repeat_ = (Repeat)(((int)repeat_ + (d < 0 ? n - 1 : 1)) % n);
      SaveSettings();
    }});
  menu_items_.push_back({"Night mode",
    [this] { return std::string(player_->NightMode() ? "On" : "Off"); },
    [this](int) { player_->SetNightMode(!player_->NightMode()); SaveSettings(); }});
  if (!bind_watch_audio_) {
    menu_items_.push_back({"Picture",
      [this] { return std::string(FitName(player_->GetFit())); },
      [this](int d) {
        const int n = (int)Player::Fit::Count;
        player_->SetFit((Player::Fit)(((int)player_->GetFit() + (d < 0 ? n - 1 : 1)) % n));
        SaveSettings();
      }});
  }
  if (!player_->SubtitleTracks().empty()) {
    menu_items_.push_back({"Subtitle delay",
      [this] { return DelayLabel(player_->SubtitleDelay()); },
      [this](int d) {
        player_->SetSubtitleDelay(d == 0 ? 0.0 : player_->SubtitleDelay() + (d < 0 ? -0.1 : 0.1));
      }});
    menu_items_.push_back({"Subtitle size",
      [this] { return std::to_string((int)std::lround(player_->SubtitleScale() * 100)) + "%"; },
      [this](int d) {
        const double scale = d == 0 ? 1.0 : player_->SubtitleScale() + (d < 0 ? -0.1 : 0.1);
        player_->SetSubtitleScale(std::clamp(scale, 0.5, 2.5));
        SaveSettings();
      }});
  }
  menu_items_.push_back({"Audio delay",
    [this] { return DelayLabel(player_->AudioDelay()); },
    [this](int d) {
      player_->SetAudioDelay(d == 0 ? 0.0 : player_->AudioDelay() + (d < 0 ? -0.05 : 0.05));
      SaveSettings();
    }});
  menu_items_.push_back({"Sleep timer",
    [this] { return SleepLabel(); },
    [this](int d) {
      // Where the timer is now among the choices, then one step on.
      int i = 0;
      if (sleep_end_of_item_) {
        i = kSleepChoiceCount - 1;
      } else if (sleep_at_ > 0) {
        const double left = (sleep_at_ - Now()) / 60.0;
        i = 1;
        while (i < kSleepChoiceCount - 2 && kSleepChoices[i] < left - 0.5)
          i++;
      }
      i = (i + (d < 0 ? kSleepChoiceCount - 1 : 1)) % kSleepChoiceCount;
      sleep_end_of_item_ = kSleepChoices[i] < 0;
      sleep_at_ = kSleepChoices[i] > 0 ? Now() + kSleepChoices[i] * 60.0 : 0.0;
    }});
  if (playing_identified_) {
    // By what the library knows of it: the menus may have moved on to
    // another source since it started.
    const bool fav = library_.IsFavorite(playing_ref_.source_key,
                                         playing_ref_.entry.id);
    menu_items_.push_back({fav ? "Remove from favorites" : "Add to favorites",
      nullptr, [this](int) {
        const bool now = library_.ToggleFavorite(playing_ref_);
        library_.Save();
        ShowToast(now ? "Added to favorites" : "Removed from favorites", icon::kStar);
      }});
  }
  if (NeighborIndex(+1) >= 0)
    menu_items_.push_back({"Play next", nullptr, [this](int) { PlayNeighbor(+1); }});
  menu_items_.push_back({"Stop", nullptr, [this](int) { StopPlayback(); }});

  sel_menu_ = 0;
  RebuildMenuRows();
  Set("menu_open", bind_menu_open_, true);
}

// The playable items of the play queue in play order: display order, or a
// shuffle of it that stays put for as long as the queue and the shuffle do
// (so "up next" is what really comes next).
const std::vector<int>& App::PlayOrder() const {
  if (queue_.entries.empty()) {
    play_order_.clear();
    play_order_key_.clear();
    return play_order_;
  }
  const browse::Listing& entries = queue_.entries;
  const std::string key = (queue_.source ? queue_.source->Key() : std::string()) + '\n' +
    queue_.id + '\n' + std::to_string(entries.size()) + '\n' +
    (entries.empty() ? std::string() : entries.front().id) + '\n' +
    (shuffle_ ? std::to_string(shuffle_seed_) : std::string("-"));
  if (key == play_order_key_)
    return play_order_;
  play_order_key_ = key;
  play_order_.clear();
  for (size_t i = 0; i < entries.size(); i++)
    if (!entries[i].IsFolder() && !entries[i].IsImage())
      play_order_.push_back((int)i);
  if (shuffle_) {
    std::mt19937 rng(shuffle_seed_);
    std::shuffle(play_order_.begin(), play_order_.end(), rng);
  }
  return play_order_;
}

void App::SetShuffle(bool on) {
  shuffle_ = on;
  // A new shuffle each time it is turned on, with whatever is playing
  // first, so the rest of the folder all still comes after it.
  shuffle_seed_ = (uint32_t)std::rand() | 1u;
  play_order_key_.clear();
  if (on && !queue_.entries.empty()) {
    const std::string& from = launch_request_ ? launch_entry_.id : playing_.id;
    const std::vector<int>& order = PlayOrder();
    for (size_t i = 0; i < order.size(); i++) {
      if (queue_.entries[order[i]].id == from) {
        std::rotate(play_order_.begin(), play_order_.begin() + i, play_order_.begin() + i + 1);
        break;
      }
    }
  }
  SaveSettings();
  ShowToast(on ? "Shuffle on" : "Shuffle off", icon::kShuffle);
}

// Finds the next (direction=+1) or previous (direction=-1) playable item
// after the one playing, in play order. Used by the track-skip keys,
// auto-advance and the "Up next" lines.
int App::NeighborIndex(int direction) const {
  if (queue_.entries.empty())
    return -1;
  const browse::Listing& entries = queue_.entries;
  const std::vector<int>& order = PlayOrder();

  // Locate the playing item by id; the selection may have moved. While a
  // launch is in flight that item is the reference, not the one on screen.
  const std::string& from = launch_request_ ? launch_entry_.id : playing_.id;
  int at = -1;
  for (size_t i = 0; i < order.size(); i++) {
    if (entries[order[i]].id == from) {
      at = (int)i;
      break;
    }
  }
  if (at < 0)
    return -1;
  int next = at + direction;
  if (repeat_ == Repeat::All && !order.empty())
    next = (next + (int)order.size()) % (int)order.size();
  if (next < 0 || next >= (int)order.size() || next == at)
    return -1;
  return order[next];
}

// Moving on within a folder resumes without asking: a question between
// two episodes (or tracks) would only interrupt.
bool App::PlayNeighbor(int direction) {
  const int i = NeighborIndex(direction);
  if (i < 0)
    return false;
  const browse::Entry entry = queue_.entries[i];
  // The cursor follows along when the queue's folder is the one on screen.
  if (view_ == View::Browse && !path_.empty() && current_source_ == queue_.source &&
      path_.back().id == queue_.id) {
    const browse::Listing& listed = path_.back().entries;
    for (size_t j = 0; j < listed.size(); j++) {
      if (listed[j].id == entry.id) {
        sel_entry_ = (int)j;
        path_.back().selection = sel_entry_;
        model_.DirtyVariable("sel_entry");
        RebuildDetail();
        scroll_entries_pending_ = true;
        break;
      }
    }
  }

  int64_t start = 0;
  library::Ref ref;
  library::Progress p;
  if (IdentifyIn(entry, queue_.source, ref) &&
      library_.GetProgress(ref.source_key, ref.entry.id, p) && !p.watched)
    start = p.position;
  Launch(entry, start, true);
  launch_direction_ = direction;
  return true;
}

void App::QueueCurrentFolder() {
  if (path_.empty())
    queue_ = PlayQueue{};
  else
    queue_ = PlayQueue{current_source_, path_.back().id, path_.back().entries};
}

size_t App::FolderPlayableCount() const {
  if (path_.empty())
    return 0;
  size_t n = 0;
  for (const browse::Entry& e : path_.back().entries)
    if (!e.IsFolder() && !e.IsImage())
      n++;
  return n;
}

void App::PlayFolder(bool shuffled) {
  QueueCurrentFolder();
  if (shuffled != shuffle_)
    SetShuffle(shuffled);
  else if (shuffled)
    SetShuffle(true); // a fresh shuffle
  const std::vector<int>& order = PlayOrder();
  if (order.empty())
    return;
  const browse::Entry entry = queue_.entries[order.front()];
  sel_entry_ = order.front(); // the queue is the folder on screen
  path_.back().selection = sel_entry_;
  model_.DirtyVariable("sel_entry");
  RebuildDetail();
  scroll_entries_pending_ = true;
  Launch(entry, 0);
}

void App::SkipChapter(int direction) {
  const std::vector<ChapterInfo> chapters = player_->Chapters();
  const int n = (int)chapters.size();
  const int i = player_->CurrentChapter();
  if (n > 1 && player_->CanSeek()) {
    if (direction > 0 && i + 1 < n) {
      player_->SeekChapter(i + 1);
      ShowToast("Chapter " + std::to_string(i + 2) + "/" + std::to_string(n) +
                (chapters[i + 1].title.empty() ? "" : ": " + chapters[i + 1].title), icon::kNext);
      return;
    }
    if (direction < 0 && i >= 0) {
      // Back to the start of this chapter, or (just after it started) the
      // one before, as a CD player does.
      const int64_t into = player_->PositionUs() - chapters[i].start_us;
      const int target = into < 3000000 && i > 0 ? i - 1 : i;
      if (into >= 3000000 || i > 0) {
        player_->SeekChapter(target);
        ShowToast("Chapter " + std::to_string(target + 1) + "/" + std::to_string(n) +
                  (chapters[target].title.empty() ? "" : ": " + chapters[target].title), icon::kPrevious);
        return;
      }
    }
  }
  PlayNeighbor(direction);
}

void App::HandlePlaybackEnd() {
  RecordProgress(true, true);
  if (sleep_end_of_item_) {
    sleep_end_of_item_ = false;
    StopPlayback();
    ShowToast("Sleep timer: playback stopped", icon::kSleep);
    return;
  }
  if (repeat_ == Repeat::One) {
    Launch(playing_, 0, true);
    return;
  }
  // A video goes on to the next episode only when that is wanted (and the
  // up-next card was not waved away).
  const bool go_on = bind_watch_audio_ || (autoplay_ && !upnext_dismissed_);
  if (go_on && PlayNeighbor(+1))
    return;
  StopPlayback();
}

void App::HandleKeyWatch(Rml::Event& event, int key) {
  // A remote's rewind and fast forward seek as left and right do, speeding
  // up the same way.
  if (key == gamepad::kKeyRewind)
    key = Rml::Input::KI_LEFT;
  else if (key == gamepad::kKeyFastForward)
    key = Rml::Input::KI_RIGHT;

  switch (key) {
  case Rml::Input::KI_LEFT:
  case Rml::Input::KI_RIGHT:
  case Rml::Input::KI_UP:
  case Rml::Input::KI_DOWN:
    if (player_->CanSeek()) {
      // A run of presses (or a held button) in one direction speeds up:
      // 10 s steps at first, then 30 s, a minute, two, five.
      const bool horizontal = key == Rml::Input::KI_LEFT || key == Rml::Input::KI_RIGHT;
      const int sign = (key == Rml::Input::KI_LEFT || key == Rml::Input::KI_DOWN) ? -1 : +1;
      if (Now() > seek_bubble_until_ || (seek_accum_us_ < 0) != (sign < 0)) {
        seek_accum_us_ = 0;
        seek_run_ = 0;
      }
      const int64_t step_s = horizontal ? kSeekSteps[std::min(seek_run_, kSeekStepCount - 1)]
                                        : kSeekLargeUs / 1000000;
      seek_run_++;
      const int64_t delta = sign * step_s * 1000000;
      player_->SeekRelative(delta);
      // How far this run of presses has jumped, Kodi style ("+30 s"), in a
      // bubble that rides on the knob.
      seek_accum_us_ += delta;
      seek_bubble_until_ = Now() + 1.5;
      const int64_t secs = std::llabs(seek_accum_us_) / 1000000;
      char label[48];
      if (secs >= 60 && secs % 60 == 0)
        std::snprintf(label, sizeof(label), "%s %c%lld min",
                      seek_accum_us_ < 0 ? icon::kRewind : icon::kForward, seek_accum_us_ < 0 ? '-' : '+',
                      (long long)(secs / 60));
      else
        std::snprintf(label, sizeof(label), "%s %c%s",
                      seek_accum_us_ < 0 ? icon::kRewind : icon::kForward, seek_accum_us_ < 0 ? '-' : '+',
                      secs >= 60 ? FormatTime(secs * 1000000).c_str()
                                 : (std::to_string(secs) + " s").c_str());
      Set("watch_seek", bind_watch_seek_, Rml::String(label));
    }
    ShowWatchInfo(kWatchInfoSec);
    break;

  case Rml::Input::KI_PRIOR: // left shoulder: previous chapter or item
    SkipChapter(-1);
    break;
  case Rml::Input::KI_NEXT:  // right shoulder: next chapter or item
    SkipChapter(+1);
    break;

  case Rml::Input::KI_RETURN:
  case Rml::Input::KI_NUMPADENTER: // play / pause; on the up-next card: play it
    if (bind_upnext_) {
      PlayNeighbor(+1);
      break;
    }
    player_->TogglePause();
    ShowWatchInfo(kWatchInfoSec);
    break;

  case Rml::Input::KI_T:     // triangle: the menus, playing on (Kodi's FullScreen)
  case Rml::Input::KI_TAB:
    SetFullscreen(false);
    break;

  case Rml::Input::KI_F1:    // playback menu
    ShowWatchInfo(kWatchInfoSec);
    OpenWatchMenu();
    break;

  case Rml::Input::KI_F4:    // a remote's display / info key: the bar, or away with it
    if (bind_info_visible_ && !bind_watch_audio_)
      Set("info_visible", bind_info_visible_, false);
    else
      ShowWatchInfo(kWatchInfoSec);
    break;

  case Rml::Input::KI_BACK:  // back: the menus, playing on; held: stop
    if (bind_upnext_) {      // ... or just away with the up-next card
      upnext_dismissed_ = true;
      Set("upnext", bind_upnext_, false);
      break;
    }
    // Only the release (HandleKeyUp) or the hold (Update) tells which.
    if (!back_held_) {
      back_held_ = true;
      back_down_at_ = Now();
    }
    break;
  case Rml::Input::KI_ESCAPE: // select / Escape: the menus, playing on
    if (bind_upnext_) {
      upnext_dismissed_ = true;
      Set("upnext", bind_upnext_, false);
      break;
    }
    SetFullscreen(false);
    break;

  case Rml::Input::KI_X:     // keyboard: stop, as in Kodi
    StopPlayback();
    break;

  default:
    if (!HandleMediaKey(key))
      return;
    break;
  }
  event.StopPropagation();
}

bool App::HandleKeyBehind(int key) {
  switch (key) {
  case Rml::Input::KI_T:     // triangle / Tab: back to full screen
  case Rml::Input::KI_TAB:
    SetFullscreen(true);
    return true;
  case Rml::Input::KI_X:     // keyboard: stop
    StopPlayback();
    ShowToast("Playback stopped", icon::kStop);
    return true;
  default:
    return HandleMediaKey(key);
  }
}

bool App::HandleMediaKey(int key) {
  if (!bind_watching_)
    return false;
  switch (key) {
  case Rml::Input::KI_PAUSE:            // a remote's pause key; again: play on
  case Rml::Input::KI_MEDIA_PLAY_PAUSE: {
    const bool was_paused = player_->IsPaused();
    player_->TogglePause();
    if (bind_fullscreen_)
      ShowWatchInfo(kWatchInfoSec);
    else
      ShowToast(was_paused ? "Playing" : "Paused", was_paused ? icon::kPlay : icon::kPause);
    return true;
  }
  case Rml::Input::KI_MEDIA_STOP:
    StopPlayback();
    ShowToast("Playback stopped", icon::kStop);
    return true;
  case Rml::Input::KI_MEDIA_NEXT_TRACK:
    SkipChapter(+1);
    return true;
  case Rml::Input::KI_MEDIA_PREV_TRACK:
    SkipChapter(-1);
    return true;
  // Behind the menus (in full screen they are left and right, see
  // HandleKeyWatch): a step either way, said in a toast.
  case gamepad::kKeyRewind:
  case gamepad::kKeyFastForward: {
    if (!player_->CanSeek())
      return true;
    const bool back = key == gamepad::kKeyRewind;
    player_->SeekRelative((back ? -kSeekSteps[0] : kSeekSteps[0]) * 1000000);
    ShowToast((back ? "-" : "+") + std::to_string(kSeekSteps[0]) + " s",
              back ? icon::kRewind : icon::kForward);
    return true;
  }
  case Rml::Input::KI_F2:    // a remote's audio key: the next audio track
    CycleAudioTrack(+1);
    if (bind_fullscreen_)
      ShowWatchInfo(kWatchInfoSec);
    return true;
  default:
    return false;
  }
}

// Back let go in full screen before it was held long enough to stop: the
// menus, playing on.
void App::HandleKeyUp(int key) {
  if (key == Rml::Input::KI_BACK)
    back_swallow_ = false;
  if (key != Rml::Input::KI_BACK || !back_held_)
    return;
  back_held_ = false;
  if (FullscreenPlayback() && Now() - back_down_at_ < kHoldStopSec)
    SetFullscreen(false);
}
