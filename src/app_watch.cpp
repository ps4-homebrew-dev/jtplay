// SPDX-License-Identifier: GPL-3.0-or-later
//
// Full-screen playback. For video the UI chrome hides and the decoded
// frames fill the screen behind a transparent document body, with an
// auto-hiding info/transport bar. For audio a persistent now-playing card
// is shown instead. The options button opens a playback menu (tracks,
// subtitles, speed) over either.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

#include "app.h"
#include "app_internal.h"

using namespace appdetail;

namespace {

constexpr double kSpeeds[] = {0.5, 0.75, 1.0, 1.25, 1.5, 2.0};
constexpr int kSpeedCount = (int)(sizeof(kSpeeds) / sizeof(kSpeeds[0]));

std::string SpeedLabel(double speed) {
  char buf[16];
  std::snprintf(buf, sizeof(buf), "%g×", speed);
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
    Set("menu_title", bind_menu_title_, Rml::String(entry.name));
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
void App::Launch(const browse::Entry& entry, int64_t start_seconds) {
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

  library::Ref ref;
  const bool identified = Identify(entry, ref);

  // Resolve and open on the worker: a source with expiring URLs mints one
  // here, as late as possible, and both steps may block on the network.
  const browse::Entry item = entry;
  const browse::SourcePtr source = current_source_;
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
      } else if (!ok) {
        ShowToast(error.empty() ? "Playback failed" : error, "⚠");
        const int direction = launch_direction_;
        const browse::Entry failed = launch_entry_;
        CancelLaunch();
        // Moving through a folder from the player: whatever was playing has
        // been stopped for this one, so carry on past it (as a playlist
        // skips a broken entry) or leave the player when the folder ends.
        if (bind_watching_) {
          playing_ = failed;
          if (direction == 0 || !PlayNeighbor(direction))
            StopPlayback();
        }
      } else {
        // The player is running: only now is there anything to look at.
        playing_ = launch_entry_;
        playing_ref_ = ref;
        playing_identified_ = identified;
        progress_saved_at_ = Now();
        watch_last_pos_us_ = watch_last_dur_us_ = -1;
        watch_last_seekable_ = false;
        CancelLaunch();
        EnterWatch(playing_);
        if (start_seconds > 0)
          ShowToast("Resumed at " + FormatTime(start_seconds * 1000000), "⏯");
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
  Set("launch_status", bind_launch_status_, Rml::String());
}

void App::EnterWatch(const browse::Entry& entry) {
  Set("watching", bind_watching_, true);
  Set("watch_audio", bind_watch_audio_, entry.IsAudio());
  Set("watch_name", bind_watch_name_,
      entry.name.empty() ? std::string("(untitled)") : entry.name);
  Set("watch_description", bind_watch_description_, entry.description);
  Set("watch_paused", bind_watch_paused_, false);
  Set("watch_pos", bind_watch_pos_, Rml::String());
  Set("watch_dur", bind_watch_dur_, Rml::String());
  Set("watch_progress", bind_watch_progress_, Rml::String("0%"));
  Set("watch_buffered", bind_watch_buffered_, Rml::String("0%"));
  Set("watch_speed", bind_watch_speed_, Rml::String());
  Set("watch_vtrack", bind_watch_vtrack_, Rml::String());
  Set("watch_atrack", bind_watch_atrack_, Rml::String());
  RefreshImageBindings();
  ShowWatchInfo(kWatchInfoSec);
}

void App::ExitWatch() {
  Set("watching", bind_watching_, false);
  Set("info_visible", bind_info_visible_, false);
  if (bind_menu_open_)
    CloseMenu();
  playing_identified_ = false;
  RefreshImageBindings();
  OnLibraryChanged(); // progress marks, Continue watching
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

  // What plays after this one, for the audio card's "Up next" line.
  Rml::String next;
  if (bind_watch_audio_ && !path_.empty()) {
    const int i = NeighborIndex(+1);
    if (i >= 0)
      next = path_.back().entries[i].name;
  }
  Set("watch_next", bind_watch_next_, next);
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
              std::to_string(tracks.size()) + ": " + tracks[next].label, "🔊");
  }
}

// Space (square) / the playback menu: next video track. Streams that carry
// several camera angles or bitrate variants show up as separate video
// streams in one container, which is what this walks.
void App::CycleVideoTrack(int direction) {
  const std::vector<VideoTrackInfo> tracks = player_->VideoTracks();
  if (tracks.size() < 2) {
    if (!tracks.empty())
      ShowToast("Only one video track");
    return;
  }
  const size_t next = NextTrack(tracks, player_->CurrentVideoStream(), direction);
  if (player_->SelectVideoTrack(tracks[next].stream_index)) {
    ShowToast("Video " + std::to_string(next + 1) + "/" +
              std::to_string(tracks.size()) + ": " + tracks[next].label, "🎬");
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
  if (player_->VideoTracks().size() > 1) {
    menu_items_.push_back({"Video",
      [this] { return player_->CurrentVideoLabel(); },
      [this](int d) { CycleVideoTrack(d < 0 ? -1 : +1); }});
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
  if (playing_identified_) {
    const bool fav = library_.IsFavorite(playing_ref_.source_key,
                                         playing_ref_.entry.id);
    const browse::Entry entry = playing_;
    menu_items_.push_back({fav ? "Remove from favorites" : "Add to favorites",
      nullptr, [this, entry](int) { ToggleFavorite(entry); }});
  }
  if (NeighborIndex(+1) >= 0)
    menu_items_.push_back({"Play next", nullptr, [this](int) { PlayNeighbor(+1); }});
  menu_items_.push_back({"Stop", nullptr, [this](int) { StopPlayback(); }});

  sel_menu_ = 0;
  RebuildMenuRows();
  Set("menu_open", bind_menu_open_, true);
}

// Finds the next (direction=+1) or previous (direction=-1) playable
// non-folder item in the current folder, in display order. Used by the
// track-skip keys, audio auto-advance and the "Up next" line.
int App::NeighborIndex(int direction) const {
  if (path_.empty())
    return -1;
  const browse::Listing& entries = path_.back().entries;

  // Locate the playing item by id; the selection may have moved. While a
  // launch is in flight that item is the reference, not the one on screen.
  const std::string& from = launch_request_ ? launch_entry_.id : playing_.id;
  int index = -1;
  for (size_t i = 0; i < entries.size(); i++) {
    if (entries[i].id == from) {
      index = (int)i;
      break;
    }
  }
  if (index < 0)
    return -1;

  for (int i = index + direction; i >= 0 && i < (int)entries.size(); i += direction) {
    if (!entries[i].IsFolder() && !entries[i].IsImage())
      return i;
  }
  return -1;
}

// Moving on within a folder resumes without asking: a question between
// two episodes (or tracks) would only interrupt.
bool App::PlayNeighbor(int direction) {
  const int i = NeighborIndex(direction);
  if (i < 0)
    return false;
  sel_entry_ = i;
  path_.back().selection = i;
  model_.DirtyVariable("sel_entry");
  RebuildDetail();
  scroll_entries_pending_ = true;

  const browse::Entry entry = path_.back().entries[i];
  int64_t start = 0;
  library::Ref ref;
  library::Progress p;
  if (Identify(entry, ref) && library_.GetProgress(ref.source_key, ref.entry.id, p) &&
      !p.watched)
    start = p.position;
  Launch(entry, start);
  launch_direction_ = direction;
  return true;
}

void App::HandlePlaybackEnd() {
  RecordProgress(true, true);
  if (PlayNeighbor(+1))
    return;
  StopPlayback();
}

void App::HandleKeyWatch(Rml::Event& event, int key) {
  switch (key) {
  case Rml::Input::KI_LEFT:
  case Rml::Input::KI_RIGHT:
  case Rml::Input::KI_UP:
  case Rml::Input::KI_DOWN:
    if (player_->CanSeek()) {
      int64_t delta = 0;
      if (key == Rml::Input::KI_LEFT)  delta = -kSeekSmallUs;
      if (key == Rml::Input::KI_RIGHT) delta = +kSeekSmallUs;
      if (key == Rml::Input::KI_UP)    delta = +kSeekLargeUs;
      if (key == Rml::Input::KI_DOWN)  delta = -kSeekLargeUs;
      player_->SeekRelative(delta);
      // How far this run of presses has jumped, Kodi style ("+30 s"), in a
      // bubble that rides on the knob.
      if (Now() > seek_bubble_until_)
        seek_accum_us_ = 0;
      seek_accum_us_ += delta;
      seek_bubble_until_ = Now() + 1.5;
      const int64_t secs = std::llabs(seek_accum_us_) / 1000000;
      char label[48];
      if (secs >= 60 && secs % 60 == 0)
        std::snprintf(label, sizeof(label), "%s %c%lld min",
                      seek_accum_us_ < 0 ? "⏪" : "⏩", seek_accum_us_ < 0 ? '-' : '+',
                      (long long)(secs / 60));
      else
        std::snprintf(label, sizeof(label), "%s %c%s",
                      seek_accum_us_ < 0 ? "⏪" : "⏩", seek_accum_us_ < 0 ? '-' : '+',
                      secs >= 60 ? FormatTime(secs * 1000000).c_str()
                                 : (std::to_string(secs) + " s").c_str());
      Set("watch_seek", bind_watch_seek_, Rml::String(label));
    }
    ShowWatchInfo(kWatchInfoSec);
    break;

  case Rml::Input::KI_PRIOR: // page up / left shoulder: previous item
    PlayNeighbor(-1);
    break;
  case Rml::Input::KI_NEXT:  // page down / right shoulder: next item
    PlayNeighbor(+1);
    break;

  case Rml::Input::KI_RETURN:
  case Rml::Input::KI_NUMPADENTER: // play / pause
    player_->TogglePause();
    ShowWatchInfo(kWatchInfoSec);
    break;

  case Rml::Input::KI_T:     // next audio track
    CycleAudioTrack();
    ShowWatchInfo(kWatchInfoSec);
    break;

  case Rml::Input::KI_SPACE: // next video track
    CycleVideoTrack();
    ShowWatchInfo(kWatchInfoSec);
    break;

  case Rml::Input::KI_F1:    // playback menu
    ShowWatchInfo(kWatchInfoSec);
    OpenWatchMenu();
    break;

  case Rml::Input::KI_BACK:  // back to the browse list
  case Rml::Input::KI_ESCAPE:
    StopPlayback();
    break;

  default:
    return;
  }
  event.StopPropagation();
}
