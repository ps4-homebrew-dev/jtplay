// SPDX-License-Identifier: GPL-3.0-or-later
//
// The app's side of the library (favorites, resume points, watched marks)
// and the screensaver.
#include <algorithm>
#include <cstdlib>

#include "app.h"
#include "app_internal.h"

using namespace appdetail;

namespace {

// Progress is remembered for video, and for audio long enough to be an
// audiobook or a podcast rather than a song.
constexpr int64_t kMinResumableAudioSec = 20 * 60;
// Positions this close to the start are not worth resuming from...
constexpr int64_t kMinResumeSec = 30;
// ...and this far through counts as watched (credits roll).
constexpr double kWatchedFraction = 0.92;

} // namespace

bool App::Identify(const browse::Entry& entry, library::Ref& out) const {
  std::string key, id;
  if (library::DecodeId(entry.id, key, id)) {
    // Listed under Favorites / Continue watching: the id carries its source.
    out.source_key = key;
    out.entry = entry;
    out.entry.id = id;
    out.source_name.clear();
    if (const browse::SourcePtr source = registry_.Find(key))
      out.source_name = source->Name();
    return true;
  }
  if (!current_source_)
    return false;
  out.source_key = current_source_->Key();
  out.source_name = current_source_->Name();
  out.entry = entry;
  return true;
}

void App::ToggleFavorite(const browse::Entry& entry) {
  library::Ref ref;
  if (!Identify(entry, ref))
    return;
  // Keep the saved copy free of what only made sense in the listing.
  if (ref.source_name.empty())
    ref.source_name = current_source_ ? current_source_->Name() : std::string();
  const bool now = library_.ToggleFavorite(ref);
  library_.Save();
  ShowToast(now ? "Added to favorites" : "Removed from favorites");
}

void App::SetWatched(const browse::Entry& entry, bool watched) {
  library::Ref ref;
  if (!Identify(entry, ref))
    return;
  library_.SetWatched(ref, watched);
  library_.Save();
  ShowToast(watched ? "Marked as watched" : "Marked as unwatched");
}

void App::RecordProgress(bool final, bool ended) {
  if (!playing_identified_)
    return;
  const int64_t dur = watch_last_dur_us_ / 1000000;
  const int64_t pos = ended ? dur : watch_last_pos_us_ / 1000000;
  // Live streams and things that cannot seek have nothing to resume.
  if (!watch_last_seekable_ || dur <= 0 || pos < 0)
    return;
  const browse::Entry& e = playing_ref_.entry;
  if (!e.IsVideo() && !(e.IsAudio() && dur >= kMinResumableAudioSec))
    return;

  if ((double)pos >= kWatchedFraction * (double)dur) {
    library_.SetWatched(playing_ref_, true);
  } else if (pos >= kMinResumeSec) {
    library_.SetResume(playing_ref_, pos, dur);
  } else {
    return; // barely started: leave whatever was recorded before
  }
  // On disk at the end, and every minute along the way, so a crash or a
  // console switched off mid-film still finds its place.
  const double now = Now();
  if (final || now - library_saved_at_ > 60.0) {
    library_saved_at_ = now;
    library_.Save();
  }
}

void App::RebuildSources() {
  const std::string selected = sel_source_ < (int)sources_.size()
    ? sources_[sel_source_]->Key() : std::string();

  favorites_source_->UpdateDetail();
  continue_source_->UpdateDetail();

  sources_.clear();
  sources_.push_back(favorites_source_);
  if (continue_source_->Count() > 0)
    sources_.push_back(continue_source_);
  sources_.insert(sources_.end(), discovered_.begin(), discovered_.end());

  for (size_t i = 0; i < sources_.size() && !selected.empty(); i++) {
    if (sources_[i]->Key() == selected) {
      sel_source_ = (int)i;
      break;
    }
  }
  RebuildSourceRows();
}

void App::OnLibraryChanged() {
  RebuildSources();
  if (view_ != View::Browse)
    return;

  // A library view lists the library itself, so it is listed again; but
  // not under a playing item, whose neighbors would move around.
  const bool library_view = current_source_ &&
    (current_source_ == favorites_source_ || current_source_ == continue_source_);
  if (library_view && path_.size() == 1 && !bind_watching_ && !launch_request_) {
    RequestBrowse(path_.back().id, path_.back().name, true);
    return;
  }
  RebuildEntryRows();
  RebuildDetail();
}

// ---------------------------------------------------------------------------
// Screensaver
// ---------------------------------------------------------------------------

void App::UpdateScreensaver(double now) {
  const bool video_running =
    bind_watching_ && !bind_watch_audio_ && !player_->IsPaused();
  if (video_running) {
    last_input_ = now; // a film is not idleness
    Set("saver", bind_saver_, false);
    return;
  }

  if (!bind_saver_) {
    if (now - last_input_ < kScreensaverSec)
      return;
    Set("saver", bind_saver_, true);
    saver_moved_at_ = 0.0;
  }

  // Drift, so nothing stays burnt into one spot of the panel.
  if (now - saver_moved_at_ > kScreensaverMoveSec) {
    saver_moved_at_ = now;
    const int x = 96 + std::rand() % (1920 - 2 * 96 - 760);
    const int y = 96 + std::rand() % (1080 - 2 * 96 - 260);
    Set("saver_x", bind_saver_x_, std::to_string(x) + "px");
    Set("saver_y", bind_saver_y_, std::to_string(y) + "px");
  }
  Set("saver_now", bind_saver_now_,
      bind_watching_ ? bind_watch_name_ : Rml::String());
}
