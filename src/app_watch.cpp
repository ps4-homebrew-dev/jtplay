// SPDX-License-Identifier: GPL-3.0-or-later
//
// Playback. It starts full screen: for video the UI chrome hides and the
// decoded frames fill the screen behind a transparent document body, with
// an auto-hiding info/transport bar; for audio a persistent now-playing
// card is shown instead. Either has a row of buttons for what playback is
// controlled with (previous, pause, next, stop, tracks, shuffle, repeat).
// The keys steer playback directly (left and right seek, up and down seek
// further) until select puts the focus on the buttons, on play / pause;
// then left and right go along them and select presses one, until back
// takes the focus off them again.
//
// An item that starts shows on a thin bar along the top for a moment
// (unless the video bar's buttons started it: that bar then stays as it
// was). The video bar along the bottom comes up by itself when playback
// pauses or plays on, and with the options key, and goes again once no
// key has been pressed for a while. Brought up by select, for its
// buttons, it stays until back (or options) sends it away -- except when
// its buttons start another item, or play on from a pause: then it goes
// as it does by itself. A seek with the bar away shows on the thin bar
// along the top, which goes a moment after the last one.
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
#include <unordered_set>

#include "app.h"
#include "app_internal.h"
#include "gamepad.h"
#include "icons.h"

using namespace appdetail;

namespace {

// Up next: the card shows over the last this many seconds of a video long
// enough to be an episode.
constexpr int64_t kUpNextSec = 15;
constexpr int64_t kUpNextMinDurationSec = 5 * 60;

// Seek steps grow while the same direction is pressed (or held) in quick
// succession: three presses of each, then the largest step from there on.
constexpr int64_t kSeekSteps[] = {10, 10, 10, 30, 30, 30, 60, 60, 120, 300};
constexpr int kSeekStepCount = (int)(sizeof(kSeekSteps) / sizeof(kSeekSteps[0]));
// Up and down: one large step.
constexpr int64_t kSeekLargeSec = 5 * 60;

const char* RepeatName(int repeat) {
  static const char* const kNames[] = {"Off", "All", "One"};
  return kNames[repeat];
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
void App::PlayEntry(const browse::Entry& entry, bool resume) {
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
    if (resume) {
      Launch(entry, at);
      return;
    }
    menu_items_.clear();
    ClearMenuButtons();
    menu_section_.clear();
    menu_parent_ = nullptr;
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
  launch_cancel_ = net::MakeCancelToken();
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
    launch_place_source_.clear();
    launch_place_.clear();
    if (view_ == View::Browse && !path_.empty()) {
      const BrowseLevel& level = path_.back();
      for (const browse::Entry& e : level.entries) {
        if (e.id == entry.id) {
          launch_queue_ = QueueOf(level);
          // And where that folder is, to be back in it to resume (a
          // folder of a source, or under a favorite; not search results).
          std::vector<library::PlaceLevel> place = CurrentPlace();
          if (current_source_ && place.size() == path_.size() &&
              (current_source_ == favorites_source_ || !LibraryView(current_source_))) {
            launch_place_source_ = current_source_->Key();
            launch_place_ = std::move(place);
          }
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
  const net::CancelToken cancel = launch_cancel_;
  PostTask(Lane::Playback, [this, item, source, request, cancel, start_seconds, ref, identified] {
    std::string error;
    browse::Playable playable;
    playable.uri = item.uri;
    bool ok;
    if (source) {
      net::CancelScope scope(cancel);
      ok = !net::Cancelled() && source->Resolve(item, playable, error);
    } else {
      ok = !playable.uri.empty();
    }
    // Given up on while resolving: the player is left as it is, with
    // whatever it plays (opening would have stopped that first).
    if (ok && cancel->load()) {
      ok = false;
      error = net::kCancelledError;
    }
    if (ok) {
      Player::Media media;
      media.url = playable.uri;
      media.headers = playable.headers;
      for (const browse::Playable::Subtitle& sub : playable.subtitles)
        media.subtitles.push_back({sub.uri, sub.language, sub.title});
      media.avoid_audio = playable.avoid_audio;
      opened_launch_ = request;
      ok = player_->Open(media, error, start_seconds);
    } else if (error.empty()) {
      error = "this item has nothing to play";
    }

    Reply([this, request, ok, error, ref, identified] {
      if (!launch_request_ || request != launch_request_) {
        // Abandoned while it was opening (back, or another item started).
        // If it did open, take it down again rather than showing it --
        // unless a launch after it has opened since (the lane runs them in
        // order): that one replaced it already, and plays.
        if (ok)
          PostTask(Lane::Playback, [this, request] {
            if (opened_launch_ == request)
              player_->Stop();
          }, false);
        // Cancelled outright, from the menus over something playing, after
        // the player had let that go to open this one: nothing plays now.
        if (!launch_request_ && bind_watching_ && (ok || !player_->IsPlaying()))
          StopPlayback();
      } else if (!ok) {
        ShowToast(error.empty() ? "Playback failed" : error, icon::kWarning);
        const int direction = launch_direction_;
        const bool queued = launch_from_queue_;
        const browse::Entry failed = launch_entry_;
        CancelLaunch();
        // Chosen from the menus over something playing, and it never got
        // as far as the player (which stops what it plays only to open the
        // next): what was playing plays on.
        if (bind_watching_ && !queued && player_->IsPlaying())
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
        if (!launch_from_queue_) {
          DropQueueMore(); // (another folder's page)
          queue_ = std::move(launch_queue_);
          playing_place_source_ = std::move(launch_place_source_);
          playing_place_ = std::move(launch_place_);
        }
        const bool behind = launch_behind_;
        CancelLaunch();
        EnterWatch(playing_, !behind);
        // The last of what is listed: the next page on its way already, so
        // that what follows it (and "Up next") is there when it ends.
        queue_more_empty_ = 0;
        if (NeighborIndex(+1) < 0)
          QueueLoadMore(false);
      }
    });
  });
}

// Cancelling stops us caring about the result, and asks the player to give
// up on an open in progress. If the open got through anyway, Update() throws
// the result away and stops the stream again.
void App::CancelLaunch() {
  if (!launch_request_)
    return;
  // Frees the lane from a slow resolve or open rather than letting it run on.
  net::Cancel(launch_cancel_);
  launch_cancel_.reset();
  player_->AbortOpen();
  launch_request_ = 0;
  launch_from_queue_ = false;
  launch_behind_ = false;
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
  Set("video_behind", bind_video_behind_, !fullscreen);
  back_held_ = false;
  Set("watch_name", bind_watch_name_, DisplayName(entry));
  Set("watch_description", bind_watch_description_, entry.description);
  Set("watch_paused", bind_watch_paused_, false);
  Set("watch_pos", bind_watch_pos_, Rml::String());
  Set("watch_dur", bind_watch_dur_, Rml::String());
  Set("watch_progress", bind_watch_progress_, Rml::String("0%"));
  Set("watch_buffered", bind_watch_buffered_, Rml::String("0%"));
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
  // The top bar shows the new item for a moment, and goes by itself,
  // however the item came to play (audio: its card, always up). Unless it
  // came from the bottom bar's buttons: that stays, the focus where it was
  // ("next", pressed again, is the one after), and goes by itself too.
  Set("seekbar_visible", bind_seekbar_visible_, false);
  if (fullscreen && (bind_watch_audio_ || (bind_info_visible_ && bind_control_focus_))) {
    PeekWatchInfo();
    info_stay_ = false;
  } else if (fullscreen) {
    HideWatchInfo();
    seekbar_deadline_ = Now() + kWatchInfoSec;
    Set("seekbar_visible", bind_seekbar_visible_, true);
  } else {
    HideWatchInfo();
    FocusControls(false);
  }
  // Home's row of what is playing: there, or now this.
  if (view_ == View::Home)
    RebuildShelves();
  if (view_ == View::Browse) {
    RebuildEntryRows(); // the mark on the row of what plays
    RebuildDetail();    // its status: now playing
  }
}

void App::ExitWatch() {
  const bool was_fullscreen = bind_fullscreen_;
  Set("watching", bind_watching_, false);
  Set("fullscreen", bind_fullscreen_, false);
  Set("video_behind", bind_video_behind_, false);
  back_held_ = false;
  Set("upnext", bind_upnext_, false);
  HideWatchInfo();
  FocusControls(false);
  Set("seekbar_visible", bind_seekbar_visible_, false);
  // A list of tracks goes with the player; a menu over the menus stays.
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
    CloseMenu(); // the playback options, or the menu over the menus
  Set("fullscreen", bind_fullscreen_, on);
  Set("video_behind", bind_video_behind_, !on);
  if (on) {
    PeekWatchInfo();
  } else {
    Set("upnext", bind_upnext_, false);
    HideWatchInfo();
    FocusControls(false);
    Set("seekbar_visible", bind_seekbar_visible_, false);
    Set("watch_seek", bind_watch_seek_, Rml::String());
    // The lists come back showing how far it has got.
    RecordProgress(false);
    progress_saved_at_ = Now();
    // The shell was hidden: put its lists back where they were.
    scroll_entries_pending_ = true;
    scroll_rows_pending_ = true;
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
  DropQueueMore();
  if (bind_watching_)
    RecordProgress(true);
  ExitWatch();
  PostTask(Lane::Playback, [this] { player_->Stop(); }, false);
}

// The focus onto the buttons of the bar (or of the now-playing card),
// which select asks for; the bar comes with it, and stays until back
// sends it away (see App::Update).
void App::ShowControls() {
  info_deadline_ = Now() + kWatchInfoSec;
  info_stay_ = true;
  Set("info_visible", bind_info_visible_, true);
  FocusControls(true);
}

// The bar, for a while (longer): back to full screen, a pause, a track
// picked with a remote's key, the options key. Up already, it is left as it is, a while longer
// (and if select brought it up, it stays). The focus is left where it is,
// which with the bar away is not on its buttons, so the keys go on
// steering playback.
void App::PeekWatchInfo() {
  info_deadline_ = Now() + kWatchInfoSec;
  if (!bind_info_visible_)
    info_stay_ = false;
  Set("info_visible", bind_info_visible_, true);
}

// Away with the bar, and the focus off its buttons. (The now-playing card
// stays; only the focus goes.)
void App::HideWatchInfo() {
  info_stay_ = false;
  Set("info_visible", bind_info_visible_, false);
  FocusControls(false);
}

void App::ShowSeek() {
  if (bind_watch_audio_ || bind_info_visible_) {
    PeekWatchInfo(); // (the card, or the bar that is up, shows it)
    return;
  }
  seekbar_deadline_ = Now() + kSeekBarSec;
  Set("seekbar_visible", bind_seekbar_visible_, true);
}

// One step of a seek, back (-1) or on (+1). A run of them in one direction
// (a held button, a button pressed again and again) speeds up: 10 s steps
// at first, then 30 s, a minute, two, five. 'large': five minutes at once.
void App::SeekStep(int sign, bool large) {
  if (!player_->CanSeek())
    return;
  if (Now() > seek_bubble_until_ || (seek_accum_us_ < 0) != (sign < 0)) {
    seek_accum_us_ = 0;
    seek_run_ = 0;
  }
  const int64_t step_s =
    large ? kSeekLargeSec : kSeekSteps[std::min(seek_run_, kSeekStepCount - 1)];
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

void App::UpdateWatchOverlay() {
  const int64_t pos = player_->PositionUs();
  const int64_t dur = player_->DurationUs();
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
  Set("watch_paused", bind_watch_paused_, player_->IsPaused());
  Set("watch_seekable", bind_watch_seekable_, player_->CanSeek());

  // The labels of the streams actually being decoded, so they follow a
  // track switch.
  Set("watch_vtrack", bind_watch_vtrack_, player_->CurrentVideoLabel());
  Set("watch_atrack", bind_watch_atrack_, player_->CurrentAudioLabel());

  // The song a station says is on, or a file's own tags; not when that is
  // what the title already reads.
  {
    Rml::String title = player_->StreamTitle();
    if (title == bind_watch_name_ || title == playing_.name)
      title.clear();
    Set("watch_stream_title", bind_watch_stream_title_, title);
  }

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

  UpdateControls();
  UpdateUpNext(pos, dur);
}

// The last seconds of an episode: a card with what comes next and a
// countdown, Netflix style. Select plays it at once, back dismisses it (and
// then the player stops at the end rather than going on).
void App::UpdateUpNext(int64_t pos, int64_t dur) {
  bool show = false;
  if (bind_fullscreen_ && !bind_watch_audio_ && autoplay_ && !upnext_dismissed_ &&
      repeat_ != Repeat::One && !launch_request_ && player_->CanSeek() &&
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

// A remote's audio key: the next audio track, wrapping around.
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
// 'toast': say what it picked (a remote's subtitle key).
void App::CycleSubtitleTrack(int direction, bool toast) {
  std::vector<SubtitleTrackInfo> choices = player_->SubtitleTracks();
  if (choices.empty()) {
    if (toast)
      ShowToast("No subtitles");
    return;
  }
  SubtitleTrackInfo off;
  off.stream_index = -1;
  off.label = "Off";
  choices.insert(choices.begin(), off);
  const size_t next = NextTrack(choices, player_->CurrentSubtitleStream(), direction);
  if (player_->SelectSubtitleTrack(choices[next].stream_index) && toast)
    ShowToast("Subtitles: " + choices[next].label);
}

// ---------------------------------------------------------------------------
// Playback controls
// ---------------------------------------------------------------------------
//
// What playback is steered with is a row of buttons in the info bar (and
// on the now-playing card), not entries of a menu: previous item, play /
// pause, next item and stop; then audio track and subtitles, where there
// is a choice; then shuffle and repeat. Select puts the focus on them, on
// play / pause to begin with; without it the keys steer playback directly. The focus is by what a button is, not
// where: buttons come and go with what plays (subtitles, a second audio
// track), and the focus stays on the one it was on.

void App::UpdateControls() {
  if (!bind_fullscreen_)
    return; // out of sight
  std::vector<Control> controls;
  std::vector<ControlRow> rows;
  auto add = [&](Control control, const char* icon, bool on = false, bool dim = false,
                 bool gap = false) {
    controls.push_back(control);
    ControlRow row;
    row.icon = icon;
    row.on = on;
    row.dim = dim;
    row.gap = gap;
    rows.push_back(row);
  };
  // Previous and next (item: chapters are L1 / R1) are always there, so
  // that the others keep their place from one item to the next; with
  // nowhere to go they are dimmed.
  add(Control::Previous, icon::kPrevious, false, NeighborIndex(-1) < 0);
  add(Control::PlayPause, player_->IsPaused() ? icon::kPlay : icon::kPause);
  add(Control::Next, icon::kNext, false, NeighborIndex(+1) < 0);
  add(Control::Stop, icon::kStop);
  bool gap = true; // the first there is of the next two starts their group
  if (player_->AudioTracks().size() > 1) {
    add(Control::Audio, icon::kAudio, false, false, gap);
    gap = false;
  }
  if (!player_->SubtitleTracks().empty()) {
    add(Control::Subtitles, icon::kSubtitles, player_->CurrentSubtitleStream() >= 0, false, gap);
    gap = false;
  }
  gap = true;
  if (PlayOrder().size() > 1) {
    add(Control::Shuffle, icon::kShuffle, shuffle_, false, gap);
    gap = false;
  }
  add(Control::Repeat, repeat_ == Repeat::One ? icon::kRepeatOne : icon::kRepeat,
      repeat_ != Repeat::Off, false, gap);

  // The button in focus went (the last file with subtitles is over): play /
  // pause, which is always there, has it.
  size_t sel = 0;
  while (sel < controls.size() && controls[sel] != control_sel_)
    sel++;
  if (sel == controls.size()) {
    control_sel_ = Control::PlayPause;
    sel = 1;
  }
  controls_ = std::move(controls);
  if (rows != control_rows_) {
    control_rows_ = std::move(rows);
    model_.DirtyVariable("controls");
  }
  Set("sel_control", bind_sel_control_, (int)sel);
  Set("control_label", bind_control_label_,
      bind_control_focus_ ? Rml::String(ControlLabel(control_sel_)) : Rml::String());
}

void App::FocusControls(bool on) {
  if (on && !bind_control_focus_)
    control_sel_ = Control::PlayPause; // where select is what it is without the bar
  Set("control_focus", bind_control_focus_, on);
  if (bind_watching_)
    UpdateControls();
}

// What the button in focus is, and where it stands: "Subtitles  ·  Off".
std::string App::ControlLabel(Control control) const {
  auto with = [](const char* what, const std::string& value) {
    return value.empty() ? std::string(what) : std::string(what) + "  ·  " + value;
  };
  switch (control) {
  case Control::Previous: {
    const int i = NeighborIndex(-1);
    return i < 0 ? "Previous" : with("Previous", DisplayName(queue_.entries[i]));
  }
  case Control::PlayPause:
    return player_->IsPaused() ? "Play" : "Pause";
  case Control::Next: {
    const int i = NeighborIndex(+1);
    return i < 0 ? "Next" : with("Next", DisplayName(queue_.entries[i]));
  }
  case Control::Stop:
    return "Stop";
  case Control::Audio:
    return with("Audio", player_->CurrentAudioLabel());
  case Control::Subtitles: {
    const int id = player_->CurrentSubtitleStream();
    for (const SubtitleTrackInfo& t : player_->SubtitleTracks())
      if (t.stream_index == id)
        return with("Subtitles", t.label);
    return with("Subtitles", "Off");
  }
  case Control::Shuffle:
    return with("Shuffle", shuffle_ ? "On" : "Off");
  case Control::Repeat:
    return with("Repeat", RepeatName((int)repeat_));
  }
  return {};
}

void App::ActivateControl(Control control) {
  switch (control) {
  case Control::Previous:
    PlayNeighbor(-1);
    break;
  case Control::PlayPause:
    player_->TogglePause();
    break;
  case Control::Next:
    PlayNeighbor(+1);
    break;
  case Control::Stop:
    StopPlayback();
    break;
  // Tracks are picked from a list of them all, on the one that is on:
  // stepping through six audio tracks to hear which is which takes a
  // reload of each.
  case Control::Audio: {
    const std::vector<AudioTrackInfo> tracks = player_->AudioTracks();
    std::vector<std::string> labels;
    int current = 0;
    for (size_t i = 0; i < tracks.size(); i++) {
      labels.push_back(tracks[i].label);
      if (tracks[i].stream_index == player_->CurrentAudioStream())
        current = (int)i;
    }
    OpenChoiceMenu("Audio", labels, current, [this, tracks](int i) {
      player_->SelectAudioTrack(tracks[i].stream_index);
    });
    break;
  }
  case Control::Subtitles: {
    const std::vector<SubtitleTrackInfo> tracks = player_->SubtitleTracks();
    std::vector<std::string> labels = {"Off"};
    int current = 0;
    for (size_t i = 0; i < tracks.size(); i++) {
      labels.push_back(tracks[i].label);
      if (tracks[i].stream_index == player_->CurrentSubtitleStream())
        current = (int)i + 1;
    }
    OpenChoiceMenu("Subtitles", labels, current, [this, tracks](int i) {
      player_->SelectSubtitleTrack(i == 0 ? -1 : tracks[i - 1].stream_index);
    });
    break;
  }
  case Control::Shuffle:
    SetShuffle(!shuffle_);
    break;
  case Control::Repeat:
    repeat_ = (Repeat)(((int)repeat_ + 1) % (int)Repeat::Count);
    SaveSettings();
    break;
  }
}

// Keys with the focus on the controls (select put it there). Any of them
// tells the bar that somebody is there: it, and the focus, stay a while
// longer.
bool App::HandleKeyControls(int key) {
  if (controls_.empty())
    return false;
  // The up-next card says what select and back do while it is up.
  const bool select = key == Rml::Input::KI_RETURN || key == Rml::Input::KI_NUMPADENTER;
  const bool back = key == Rml::Input::KI_BACK || key == Rml::Input::KI_ESCAPE;
  if (bind_upnext_ && (select || back))
    return false;

  switch (key) {
  case Rml::Input::KI_LEFT:
  case Rml::Input::KI_RIGHT: {
    // Along the row, stopping at its ends.
    const int n = (int)controls_.size();
    const int to = std::clamp(bind_sel_control_ + (key == Rml::Input::KI_LEFT ? -1 : +1), 0, n - 1);
    control_sel_ = controls_[to];
    break;
  }
  case Rml::Input::KI_RETURN:
  case Rml::Input::KI_NUMPADENTER: {
    const bool resume = control_sel_ == Control::PlayPause && player_->IsPaused();
    ActivateControl(control_sel_);
    // Playing on from a pause: the bar has done what it was up for, and
    // goes by itself, as when an item starts.
    if (resume && FullscreenPlayback()) {
      UpdateControls();
      info_deadline_ = Now() + kWatchInfoSec;
      info_stay_ = false;
      return true;
    }
    break;
  }
  case Rml::Input::KI_HOME:  // the first and the last of them, as in a list
  case Rml::Input::KI_END:
    control_sel_ = key == Rml::Input::KI_HOME ? controls_.front() : controls_.back();
    break;
  case Rml::Input::KI_UP:
  case Rml::Input::KI_DOWN:
    break; // nothing above or below them
  case Rml::Input::KI_BACK:  // off the buttons (and away with the bar)
  case Rml::Input::KI_ESCAPE:
    HideWatchInfo();
    return true;
  default:
    return false;
  }
  // (Stop, or the end of the queue, has left the player.)
  if (FullscreenPlayback()) {
    info_deadline_ = Now() + kWatchInfoSec;
    UpdateControls();
  }
  return true;
}

// The playable items of the play queue in play order: display order, or a
// shuffle of it that stays put for as long as the queue and the shuffle do
// (so "up next" is what really comes next).
std::string App::PlayOrderKey() const {
  const browse::Listing& entries = queue_.entries;
  return (queue_.source ? queue_.source->Key() : std::string()) + '\n' +
    queue_.id + '\n' + std::to_string(entries.size()) + '\n' +
    (entries.empty() ? std::string() : entries.front().id) + '\n' +
    (shuffle_ ? std::to_string(shuffle_seed_) : std::string("-"));
}

const std::vector<int>& App::PlayOrder() const {
  if (queue_.entries.empty()) {
    play_order_.clear();
    play_order_key_.clear();
    return play_order_;
  }
  const browse::Listing& entries = queue_.entries;
  const std::string key = PlayOrderKey();
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
  // (Round to the first again only once there is no more to come.)
  if (repeat_ == Repeat::All && !order.empty() && (direction < 0 || queue_.next.empty()))
    next = (next + (int)order.size()) % (int)order.size();
  // Wrapped round onto itself: the only playable item. Repeat All plays it
  // again; otherwise there is no neighbour.
  if (next == at && repeat_ == Repeat::All && direction > 0)
    return order[next];
  if (next < 0 || next >= (int)order.size() || next == at)
    return -1;
  return order[next];
}

// Moving on within a folder resumes without asking: a question between
// two episodes (or tracks) would only interrupt.
bool App::PlayNeighbor(int direction) {
  const int i = NeighborIndex(direction);
  // Past the last of what is listed, with more to come: on into that, once
  // it is here. (A few pages that have nothing to play are given up on.)
  if (i < 0 && direction > 0 && !queue_.next.empty() && queue_more_empty_ < kQueueEmptyPages &&
      (launch_request_ || !playing_.id.empty())) {
    if (!queue_more_request_ || !queue_more_advance_)
      ShowToast("Loading more\u2026", icon::kMore);
    QueueLoadMore(true);
    return true;
  }
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
  DropQueueMore();
  if (path_.empty())
    queue_ = PlayQueue{};
  else
    queue_ = QueueOf(path_.back());
}

void App::DropQueueMore() {
  if (queue_more_cancel_)
    net::Cancel(queue_more_cancel_);
  queue_more_cancel_.reset();
  queue_more_request_ = 0;
  queue_more_advance_ = false;
}

App::PlayQueue App::QueueOf(const BrowseLevel& level) const {
  PlayQueue q{current_source_, level.id, level.entries};
  q.next = level.next;
  q.query = level.query;
  q.folder = level.folder;
  return q;
}

void App::QueueLoadMore(bool advance) {
  if (queue_.next.empty() || !queue_.source)
    return;
  if (advance)
    queue_more_advance_ = true;
  if (queue_more_request_)
    return; // on its way already
  const uint32_t request = queue_more_request_ = ++queue_more_counter_;
  if (queue_more_request_ == 0)
    queue_more_request_ = ++queue_more_counter_; // (0 is "none")
  const net::CancelToken cancel = queue_more_cancel_ = net::MakeCancelToken();
  const browse::SourcePtr source = queue_.source;
  const std::string id = queue_.id, cursor = queue_.next;
  const std::string query = queue_.query, folder = queue_.folder;

  PostTask(Lane::Browse, [this, request, cancel, source, id, cursor, query, folder] {
    browse::Listing result;
    std::string next, error;
    {
      net::CancelScope scope(cancel);
      if (!net::Cancelled()) {
        if (query.empty())
          source->BrowsePage(id, cursor, result, next, error);
        else if (!source->SearchPage(folder, query, cursor, result, next, error) && error.empty())
          error = "the search failed";
      }
    }
    Reply([this, request, source, id, cursor, query, result = std::move(result), next,
           error]() mutable {
      if (request != queue_more_request_)
        return;
      queue_more_request_ = 0;
      queue_more_cancel_.reset();
      const bool play_on = queue_more_advance_;
      queue_more_advance_ = false;
      // Still the queue it was fetched for: not another folder started
      // meanwhile, nor this one's next page fetched some other way.
      if (queue_.source != source || queue_.id != id || queue_.query != query ||
          queue_.next != cursor)
        return;
      // Waiting at the end of an item (not over something still playing,
      // nor a launch since): with nothing to go on to, the player stops.
      const auto stop_if_ended = [this] {
        if (bind_watching_ && !launch_request_ && !player_->IsPlaying())
          StopPlayback();
      };
      if (!error.empty()) {
        if (play_on) {
          ShowToast(error, icon::kWarning);
          stop_if_ended();
        }
        return;
      }

      // Added on behind what is there (whatever the order on screen: it
      // is what comes next); what is there already is left out.
      std::unordered_set<std::string> seen;
      for (const browse::Entry& e : queue_.entries)
        seen.insert(e.id);
      const size_t before = queue_.entries.size();
      const bool fresh_order = play_order_key_ == PlayOrderKey();
      for (const browse::Entry& e : result)
        if (seen.insert(e.id).second)
          queue_.entries.push_back(e);
      const size_t added = queue_.entries.size() - before;
      queue_.next = added == 0 && next == cursor ? std::string() : next;
      // The play order grows the same way: shuffled, the new page is a
      // shuffle of its own after the rest, so nothing plays twice.
      if (fresh_order && added > 0) {
        std::vector<int> more;
        for (size_t i = before; i < queue_.entries.size(); i++)
          if (!queue_.entries[i].IsFolder() && !queue_.entries[i].IsImage())
            more.push_back((int)i);
        if (shuffle_) {
          std::mt19937 rng(shuffle_seed_ ^ (uint32_t)before);
          std::shuffle(more.begin(), more.end(), rng);
        }
        play_order_.insert(play_order_.end(), more.begin(), more.end());
        play_order_key_ = PlayOrderKey();
      }

      // The same folder on screen, at the same page: its list grows too.
      if (view_ == View::Browse && !path_.empty() && current_source_ == source) {
        BrowseLevel& here = path_.back();
        if (here.id == id && here.query == query && here.next == cursor) {
          const std::string keep = sel_entry_ >= 0 && sel_entry_ < (int)here.entries.size()
                                 ? here.entries[sel_entry_].id : std::string();
          std::unordered_set<std::string> listed;
          for (const browse::Entry& e : here.raw)
            listed.insert(e.id);
          for (const browse::Entry& e : result)
            if (listed.insert(e.id).second)
              here.raw.push_back(e);
          here.next = queue_.next;
          NoteSourceArt(current_source_, here.raw);
          SortLevel(here);
          here.selection = std::min(sel_entry_, std::max(0, (int)here.entries.size() - 1));
          for (size_t i = 0; i < here.entries.size() && !keep.empty(); i++)
            if (here.entries[i].id == keep)
              here.selection = (int)i;
          sel_entry_ = here.selection;
          RebuildEntryRows();
          RebuildDetail();
        }
      }

      bool playable = false;
      for (size_t i = before; i < queue_.entries.size(); i++)
        playable = playable || (!queue_.entries[i].IsFolder() && !queue_.entries[i].IsImage());
      queue_more_empty_ = playable ? 0 : queue_more_empty_ + 1;
      if (play_on) {
        if (!PlayNeighbor(+1))
          stop_if_ended();
      } else if (!playable && NeighborIndex(+1) < 0 && queue_more_empty_ < kQueueEmptyPages) {
        QueueLoadMore(false); // nothing to play on that page: the one after
      }
    });
  }, false);
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
  // The focus is on the controls: the d-pad, select and back are theirs.
  // (Not the keys of a remote that seek, skip or pause whatever has the
  // focus: those go on below.)
  if (bind_control_focus_ && HandleKeyControls(key)) {
    event.StopPropagation();
    return;
  }

  // A remote's rewind and fast forward seek as left and right do, speeding
  // up the same way.
  if (key == gamepad::kKeyRewind)
    key = Rml::Input::KI_LEFT;
  else if (key == gamepad::kKeyFastForward)
    key = Rml::Input::KI_RIGHT;

  switch (key) {
  case Rml::Input::KI_LEFT:  // seek, faster and faster
  case Rml::Input::KI_RIGHT:
    SeekStep(key == Rml::Input::KI_LEFT ? -1 : +1);
    ShowSeek();
    break;
  case Rml::Input::KI_UP:    // seek five minutes on, or back
  case Rml::Input::KI_DOWN:
    SeekStep(key == Rml::Input::KI_DOWN ? -1 : +1, true);
    ShowSeek();
    break;

  case gamepad::kKeyShoulderLeft:  // L1: previous chapter or item
    SkipChapter(-1);
    break;
  case gamepad::kKeyShoulderRight: // R1: next chapter or item
    SkipChapter(+1);
    break;
  // A remote's channel up and down, which arrive as PageUp and PageDown:
  // up is the next item, down the one before, as channels go (and as
  // PageUp and PageDown are in Kodi). By item, not chapter.
  case Rml::Input::KI_PRIOR:
    PlayNeighbor(+1);
    break;
  case Rml::Input::KI_NEXT:
    PlayNeighbor(-1);
    break;

  case Rml::Input::KI_RETURN:
  case Rml::Input::KI_NUMPADENTER: // onto the buttons; on the up-next card: play it
    if (bind_upnext_) {
      PlayNeighbor(+1);
      break;
    }
    // (Not a pause: the focus goes to that button, and it takes a second
    // press.)
    ShowControls();
    break;

  // Square, and a keyboard's space bar: play / pause at once, which
  // select no longer is (it goes to the buttons).
  case Rml::Input::KI_SPACE:
    player_->TogglePause();
    PeekWatchInfo();
    break;

  case Rml::Input::KI_T:     // triangle: the menus, playing on (Kodi's FullScreen)
  case Rml::Input::KI_TAB:
    SetFullscreen(false);
    break;

  case gamepad::kKeyOptions: // the bar; again: away with it
  case Rml::Input::KI_F4:    // (F4 likewise where it is not options: SDL 3, see
                             // RmlSDL::ConvertKey)
    if (bind_watch_audio_)
      break;                 // the card is always there
    if (bind_info_visible_)
      HideWatchInfo();
    else
      PeekWatchInfo();
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
  case Rml::Input::KI_X:     // keyboard: stop
    StopPlayback();
    return true;
  default:
    return HandleMediaKey(key);
  }
}

// The keys of those that only ever mean playback (pause, stop, previous,
// next), for where the other keys mean something else: an open menu, a
// text field. True if 'key' was one, and something plays.
bool App::HandleTransportKey(int key) {
  switch (key) {
  case Rml::Input::KI_PAUSE:
  case Rml::Input::KI_MEDIA_PLAY_PAUSE:
  case Rml::Input::KI_MEDIA_STOP:
  case Rml::Input::KI_MEDIA_NEXT_TRACK:
  case Rml::Input::KI_MEDIA_PREV_TRACK:
    return HandleMediaKey(key);
  default:
    return false;
  }
}

bool App::HandleMediaKey(int key) {
  if (!bind_watching_)
    return false;
  switch (key) {
  case Rml::Input::KI_PAUSE:            // a remote's pause key; again: play on
  case Rml::Input::KI_MEDIA_PLAY_PAUSE: {
    player_->TogglePause();
    if (bind_fullscreen_)
      PeekWatchInfo(); // (behind the menus, the card in the corner says so)
    return true;
  }
  case Rml::Input::KI_MEDIA_STOP:
    StopPlayback();
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
      PeekWatchInfo();
    return true;
  case Rml::Input::KI_F1:    // a remote's subtitle key: the next subtitle track
    CycleSubtitleTrack(+1, true);
    if (bind_fullscreen_)
      PeekWatchInfo();
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
