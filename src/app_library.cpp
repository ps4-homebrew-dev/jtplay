// SPDX-License-Identifier: GPL-3.0-or-later
//
// The app's side of the library (favorites, resume points, watched marks)
// and the screensaver.
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <functional>

#include "app.h"
#include "app_internal.h"
#include "icons.h"

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
  return IdentifyIn(entry, current_source_, out);
}

bool App::IdentifyIn(const browse::Entry& entry, const browse::SourcePtr& source,
                     library::Ref& out) const {
  std::string key, id;
  if (library::DecodeId(entry.id, key, id)) {
    // Listed under Favorites / Continue watching: the id carries its source.
    out.source_key = key;
    out.entry = entry;
    out.entry.id = id;
    out.source_name.clear();
    if (const browse::SourcePtr known = registry_.Find(key))
      out.source_name = known->Name();
    else // not searched for yet: what the library remembers it as
      out.source_name = library_.SourceName(key);
    return true;
  }
  if (!source)
    return false;
  out.source_key = source->Key();
  out.source_name = source->Name();
  out.entry = entry;
  return true;
}

std::string App::ArtUri(const browse::Entry& entry) const {
  if (!entry.image.empty())
    return entry.image;
  // A source pinned before its picture was kept with it: as it is now.
  if (entry.IsFolder())
    return SourceFavoriteArt(entry);
  library::Ref ref;
  if (!Identify(entry, ref))
    return {};
  return library_.ImageFor(ref.source_key, ref.entry.id);
}

library::Ref App::SourceRef(const browse::SourcePtr& source) const {
  library::Ref ref;
  ref.source_key = source->Key();
  ref.source_name = source->Name();
  ref.entry.id = source->RootId();
  ref.entry.type = browse::Entry::Type::Folder;
  ref.entry.name = source->Name();
  ref.entry.description = source->Detail();
  // Its picture, as on the list of sources: its own logo, else artwork
  // seen in it.
  ref.entry.image = source->Image();
  library::KnownSource known;
  if (ref.entry.image.empty() && library_.FindKnownSource(ref.source_key, known))
    ref.entry.image = known.image;
  return ref;
}

std::string App::SourceFavoriteArt(const browse::Entry& entry, bool* logo) const {
  if (logo)
    *logo = false;
  std::string key, id;
  if (!entry.IsFolder() || !library::DecodeId(entry.id, key, id))
    return {};
  // Its top folder: by the source's root id once it is found; until then,
  // by the name the library knows it by. A saved search of it shows it too
  // (what it searches is said by its name).
  library::KnownSource known;
  const bool remembered = library_.FindKnownSource(key, known);
  std::string folder, query;
  const bool search = library::ParseSavedSearchId(id, folder, query);
  std::string own;
  if (const browse::SourcePtr source = registry_.Find(key)) {
    if (id != source->RootId() && !search)
      return {};
    own = source->Image();
  } else if (remembered && (search || entry.name == known.name)) {
    own = known.logo;
  } else {
    return {};
  }
  if (!own.empty()) {
    if (logo)
      *logo = true;
    return own;
  }
  return remembered ? known.image : std::string();
}

bool App::IsSnapshot(const std::string& image) const {
  const std::string prefix = "file://" + cache_dir_ + "/snap-";
  return !cache_dir_.empty() && image.compare(0, prefix.size(), prefix) == 0;
}

void App::DropSnapshot(const std::string& image) const {
  if (IsSnapshot(image))
    ::remove(image.substr(7).c_str());
}

void App::SetWatched(const browse::Entry& entry, bool watched) {
  library::Ref ref;
  if (!Identify(entry, ref))
    return;
  // Either way the resume point goes, and with it the frame it showed.
  DropSnapshot(library_.ImageFor(ref.source_key, ref.entry.id));
  library_.SetWatched(ref, watched);
  SaveLibrary();
}

void App::RecordRecent() {
  // Never got going: nothing to come back to.
  if (!playing_identified_ || watch_last_pos_us_ < 0)
    return;
  library_.AddRecent(playing_ref_);
  SaveLibrary();
}

void App::RecordProgress(bool final, bool ended) {
  if (!playing_identified_)
    return;
  if (final)
    RecordRecent();
  const int64_t dur = watch_last_dur_us_ / 1000000;
  const int64_t pos = ended ? dur : watch_last_pos_us_ / 1000000;
  // Live streams and things that cannot seek have nothing to resume.
  if (!watch_last_seekable_ || dur <= 0 || pos < 0)
    return;
  const browse::Entry& e = playing_ref_.entry;
  if (!e.IsVideo() && !(e.IsAudio() && dur >= kMinResumableAudioSec))
    return;

  // A video without artwork of its own gets the frame it was stopped on,
  // so its Continue watching card shows where it left off. One file per
  // item and position: the thumbnail cache keys on the name.
  const bool own_art = !playing_ref_.entry.image.empty() &&
                       !IsSnapshot(playing_ref_.entry.image);
  auto drop_snapshot = [&] {
    if (!own_art) {
      DropSnapshot(playing_ref_.entry.image);
      playing_ref_.entry.image.clear();
    }
  };

  if ((double)pos >= kWatchedFraction * (double)dur) {
    drop_snapshot();
    library_.SetWatched(playing_ref_, true);
  } else if (pos >= kMinResumeSec) {
    library_.SetResume(playing_ref_, pos, dur);
    if (final && !own_art && e.IsVideo() && !cache_dir_.empty()) {
      char name[64];
      std::snprintf(name, sizeof(name), "%016zx-%lld.jpg",
                    std::hash<std::string>()(playing_ref_.source_key + '\n' +
                                             playing_ref_.entry.id),
                    (long long)pos);
      const std::string path = cache_dir_ + "/snap-" + name;
      const library::Ref ref = playing_ref_;
      const std::string was = ref.entry.image;
      // The frame on disk: it becomes the card's artwork in place of the
      // last one (unless the item has changed meanwhile), which goes.
      auto apply = [this, ref, was, path] {
        const std::string image = "file://" + path;
        if (!library_.ReplaceResumeImage(ref.source_key, ref.entry.id, was, image)) {
          ::remove(path.c_str());
          return;
        }
        DropSnapshot(was);
        if (playing_identified_ && playing_ref_.source_key == ref.source_key &&
            playing_ref_.entry.id == ref.entry.id && playing_ref_.entry.image == was)
          playing_ref_.entry.image = image;
        SaveLibrary();
      };
      // On the playback lane: mpv encodes the frame there, not in this
      // frame of the UI (a JPEG of a 4K picture is a good part of a
      // second on a console), and still before the stop or the next open
      // posted after this. With the lanes stopped (quitting), here.
      const bool posted = lanes_[(int)Lane::Playback].Post([this, path, apply] {
        if (player_->Snapshot(path))
          Reply(apply);
      });
      if (!posted && player_->Snapshot(path))
        apply();
    }
    // The folder it plays from, on it (moving on through the folder, on
    // the one it has moved on to).
    if (!playing_place_.empty()) {
      std::vector<library::PlaceLevel> place = playing_place_;
      place.back().selected = playing_.id;
      library_.SetResumePlace(playing_ref_.source_key, playing_ref_.entry.id,
                              playing_place_source_, place);
    }
  } else {
    return; // barely started: leave whatever was recorded before
  }
  // On disk at the end, and every minute along the way, so a crash or a
  // console switched off mid-film still finds its place.
  const double now = Now();
  if (final || now - library_saved_at_ > 60.0) {
    library_saved_at_ = now;
    SaveLibrary();
  }
}

void App::OnLibraryChanged() {
  RebuildHome();
  if (view_ != View::Browse)
    return;

  // A folder of favorites: listed again, here and now (favorites moved,
  // added or removed, folders made, renamed or deleted).
  if (InFavoritesTree()) {
    RefreshFavoritesLevel();
    return;
  }
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
  // Anything playing, audio included, means someone is using the app.
  const bool media_playing =
    (bind_watching_ && !player_->IsPaused()) ||
    (bind_photo_ && bind_photo_playing_);
  if (media_playing) {
    last_input_ = now; // playback is not idleness
    Set("saver", bind_saver_, false);
    return;
  }

  if (!bind_saver_) {
    if (screensaver_sec_ <= 0 || now - last_input_ < screensaver_sec_)
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
