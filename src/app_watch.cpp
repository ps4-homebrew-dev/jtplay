// SPDX-License-Identifier: GPL-3.0-or-later
//
// Full-screen playback. For video the UI chrome hides and the decoded
// frames fill the screen behind a transparent document body, with an
// auto-hiding info/transport bar. For audio a persistent now-playing card
// is shown instead.
#include <algorithm>
#include <cstdio>

#include "app.h"
#include "app_internal.h"

using namespace appdetail;

// Starts an item. Nothing about the UI changes here: the browse list stays
// up -- and with it the topbar busy indicator -- until the worker reports
// that the player is running. Entering watch mode any earlier hides the
// shell, and with it the only sign of life, for as long as the resolve and
// the open take, which on a remote source is most of the wait.
void App::PlayEntry(const browse::Entry& entry) {
  CancelLaunch();

  launch_entry_ = entry;
  launch_request_ = ++launch_seq_;
  launch_phase_ = kLaunchResolving;
  launch_visible_at_ = Now() + kLaunchFeedbackSec;

  // Resolve and open on the worker: a source with expiring URLs mints one
  // here, as late as possible, and both steps may block on the network.
  const browse::Entry item = entry;
  const browse::SourcePtr source = current_source_;
  const uint32_t request = launch_request_;
  busy_ops_++;
  PostTask([this, item, source, request] {
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
      ok = player_->Open(uri, error);
    } else if (error.empty()) {
      error = "this item has nothing to play";
    }

    Reply([this, request, ok, error] {
      if (!launch_request_ || request != launch_request_) {
        // Abandoned while it was opening (back, or another item started).
        // If it did open, take it down again rather than showing it.
        if (ok)
          PostTask([this] { player_->Stop(); });
      } else if (!ok) {
        ShowToast(error.empty() ? "Playback failed" : error);
        CancelLaunch();
      } else {
        // The player is running: only now is there anything to look at.
        playing_ = launch_entry_;
        CancelLaunch();
        EnterWatch(playing_);
      }
    });
    busy_ops_--;
  });
}

// The open itself is not interruptible, so cancelling only stops us caring:
// the task runs to completion and Update() throws the result away (and
// stops the stream again if it did open).
void App::CancelLaunch() {
  if (!launch_request_)
    return;
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
  Set("watch_time", bind_watch_time_, Rml::String());
  Set("watch_progress", bind_watch_progress_, Rml::String("0%"));
  Set("watch_ends", bind_watch_ends_, Rml::String());
  Set("watch_vtrack", bind_watch_vtrack_, Rml::String());
  Set("watch_atrack", bind_watch_atrack_, Rml::String());
  RefreshImageBindings();
  ShowWatchInfo(kWatchInfoSec);
}

void App::ExitWatch() {
  Set("watching", bind_watching_, false);
  Set("info_visible", bind_info_visible_, false);
  RefreshImageBindings();
}

void App::StopPlayback() {
  CancelLaunch();
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

  Rml::String time;
  Rml::String ends;
  Rml::String progress = "0%";
  if (pos >= 0) {
    time = FormatTime(pos);
    if (dur > 0) {
      time += " / " + FormatTime(dur);
      // Fractional percentages keep the bar moving smoothly on long files.
      const double pct = std::clamp((double)pos * 100.0 / (double)dur, 0.0, 100.0);
      char buf[16];
      std::snprintf(buf, sizeof(buf), "%.2f%%", pct);
      progress = buf;
      if (dur > pos)
        ends = "Ends at " + ClockIn((dur - pos) / 1000000);
    }
  }
  Set("watch_time", bind_watch_time_, time);
  Set("watch_ends", bind_watch_ends_, ends);
  Set("watch_progress", bind_watch_progress_, progress);
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

// T (triangle): switches to the next audio track (wrapping around) and
// announces the choice with a toast.
void App::CycleAudioTrack() {
  const std::vector<AudioTrackInfo> tracks = player_->AudioTracks();
  if (tracks.size() < 2) {
    if (!tracks.empty())
      ShowToast("Only one audio track");
    return;
  }

  const int cur = player_->CurrentAudioStream();
  size_t i = 0;
  while (i < tracks.size() && tracks[i].stream_index != cur)
    i++;
  const size_t next = (i + 1) % tracks.size(); // cur not found -> wraps to 0

  if (player_->SelectAudioTrack(tracks[next].stream_index)) {
    ShowToast("Audio " + std::to_string(next + 1) + "/" +
              std::to_string(tracks.size()) + ": " + tracks[next].label);
  }
}

// Space (square): switches to the next video track (wrapping around). Streams that
// carry several camera angles or bitrate variants show up as separate video
// streams in one container, which is what this walks.
void App::CycleVideoTrack() {
  const std::vector<VideoTrackInfo> tracks = player_->VideoTracks();
  if (tracks.size() < 2) {
    if (!tracks.empty())
      ShowToast("Only one video track");
    return;
  }

  const int cur = player_->CurrentVideoStream();
  size_t i = 0;
  while (i < tracks.size() && tracks[i].stream_index != cur)
    i++;
  const size_t next = (i + 1) % tracks.size(); // cur not found -> wraps to 0

  if (player_->SelectVideoTrack(tracks[next].stream_index)) {
    ShowToast("Video " + std::to_string(next + 1) + "/" +
              std::to_string(tracks.size()) + ": " + tracks[next].label);
  }
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

bool App::PlayNeighbor(int direction) {
  const int i = NeighborIndex(direction);
  if (i < 0)
    return false;
  sel_entry_ = i;
  path_.back().selection = i;
  model_.DirtyVariable("sel_entry");
  RebuildDetail();
  scroll_entries_pending_ = true;
  PlayEntry(path_.back().entries[i]);
  return true;
}

void App::HandlePlaybackEnd() {
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
      ShowWatchInfo(kWatchInfoSec);
    }
    break;

  case Rml::Input::KI_PRIOR: // page up / left shoulder: previous track
    PlayNeighbor(-1);
    break;
  case Rml::Input::KI_NEXT:  // page down / right shoulder: next track
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

  case Rml::Input::KI_F1:    // toggle the info bar
    if (bind_info_visible_ && !bind_watch_audio_) {
      bind_info_visible_ = false;
      model_.DirtyVariable("info_visible");
    } else {
      ShowWatchInfo(kWatchInfoSec);
    }
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
