// SPDX-License-Identifier: GPL-3.0-or-later
//
// The user's own library on top of the browsable sources: favorites (any
// folder or item, from any source), playback progress (resume points and
// watched marks), what was played lately, and the sources seen so far
// (so Home can list them before they are found again). All of it is
// remembered across restarts in one small XML file in the cache directory.
//
// Everything is stored against a Ref: the key of the source an entry came
// from (browse::Source::Key()) and the entry as that source listed it. The
// LibrarySource below turns the stored refs back into a browsable source
// ("Favorites", "Continue watching") by handing every browse and resolve to
// the source the ref names, as long as that source is around.
#ifndef LIBRARY_H
#define LIBRARY_H

#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "browse/source.h"

namespace library {

// An entry together with the source that knows what its id means.
struct Ref {
  std::string source_key;
  std::string source_name;  // for display when the source is offline
  browse::Entry entry;      // entry.id is the source's own id
};

// A source seen before: enough to show its tile before it is found again,
// and how often it was opened, for the order of the tiles.
struct KnownSource {
  std::string key;
  std::string name;
  std::string detail;
  std::string icon;     // a glyph of icons.h
  int uses = 0;
  int64_t used = 0;     // epoch seconds it was last opened, 0 = never
};

// Something played lately: a live stream as itself, anything else as the
// folder it was played from (where the next one is). See App::RecordRecent.
struct Recent {
  Ref ref;
  bool live = false;
  int64_t updated = 0;  // epoch seconds
};

// Where a source was left: the folders down to the one on screen, and the
// entry selected in each (by id, as the listing may have changed).
struct PlaceLevel {
  std::string id;
  std::string name;
  std::string selected;
};

struct Progress {
  int64_t position = 0;     // seconds; 0 = start from the beginning
  int64_t duration = 0;     // seconds, 0 = unknown
  bool watched = false;
  int64_t updated = 0;      // epoch seconds, for "most recent first"
};

// Thread safe: the main thread updates it, the worker reads it while
// listing a LibrarySource.
class Library {
public:
  // Reads 'path' if it exists; later Save()s write there.
  void Load(const std::string& path);
  // Writes the file if anything changed since the last save.
  void Save();

  bool IsFavorite(const std::string& source_key, const std::string& id) const;
  // Adds or removes; returns whether it is a favorite now.
  bool ToggleFavorite(const Ref& ref);
  std::vector<Ref> Favorites() const;
  // The name the library remembers for a source, "" if it has none.
  std::string SourceName(const std::string& source_key) const;

  bool GetProgress(const std::string& source_key, const std::string& id,
                   Progress& out) const;
  // Records a resume point (position/duration in seconds).
  void SetResume(const Ref& ref, int64_t position, int64_t duration);
  // Marks watched (drops the resume point) or clears both.
  void SetWatched(const Ref& ref, bool watched);
  // Artwork remembered for an item (a snapshot of where it was stopped),
  // or "".
  std::string ImageFor(const std::string& source_key, const std::string& id) const;
  // Unfinished items, most recently played first.
  std::vector<std::pair<Ref, Progress>> InProgress() const;

  // Lately played (see Recent), most recent first. Adding one already
  // there moves it to the front.
  void AddRecent(const Ref& ref, bool live);
  void RemoveRecent(const std::string& source_key, const std::string& id);
  std::vector<Recent> RecentItems() const;

  // The sources seen: most opened first, then the order they were found
  // in. Remember adds one or updates what it says about itself; Used
  // counts an opening.
  void RememberSource(const KnownSource& source);
  void SourceUsed(const std::string& key);
  void ForgetSource(const std::string& key);
  std::vector<KnownSource> KnownSources() const;

  // Where each source was left, to open it there again; an empty list
  // forgets it. Not a change of the library as views see it (no new
  // generation): nothing on screen shows it.
  void SetPlace(const std::string& source_key, const std::vector<PlaceLevel>& levels);
  std::vector<PlaceLevel> Place(const std::string& source_key) const;

  // Bumped on every change, so views can tell when to refresh.
  uint64_t Generation() const;

private:
  struct Item {
    Ref ref;
    Progress progress;
  };
  static std::string Id(const std::string& key, const std::string& id) {
    return key + '\n' + id;
  }
  void Changed(); // with mutex_ held

  mutable std::mutex mutex_;
  std::string path_;
  bool dirty_ = false;
  uint64_t generation_ = 0;
  std::vector<Ref> favorites_;          // in the order they were added
  std::map<std::string, Item> progress_;
  std::vector<Recent> recent_;          // most recent first
  std::vector<KnownSource> sources_;    // in the order they were found
  std::map<std::string, std::vector<PlaceLevel>> places_; // by source key
};

// The real sources by key, so a LibrarySource can find the one a ref
// names. The main thread replaces the set after every discovery; the
// worker looks things up.
class SourceRegistry {
public:
  void Set(const std::vector<browse::SourcePtr>& sources);
  browse::SourcePtr Find(const std::string& key) const;
  std::vector<browse::SourcePtr> All() const;

private:
  mutable std::mutex mutex_;
  std::map<std::string, browse::SourcePtr> sources_;
};

// Entry ids inside a LibrarySource carry the source key along, so that
// everything below a favorite folder still knows where it came from.
std::string EncodeId(const std::string& source_key, const std::string& id);
bool DecodeId(const std::string& encoded, std::string& source_key, std::string& id);

class LibrarySource : public browse::Source {
public:
  // Search: lists nothing; searched at its root, it searches the library
  // (by name) and every source that can search, and lists what they found.
  enum class Kind { Favorites, ContinueWatching, Search };

  LibrarySource(Kind kind, Library& library, SourceRegistry& registry);

  const std::string& Name() const override { return name_; }
  const std::string& Detail() const override { return detail_; }
  const char* Icon() const override;
  std::string Key() const override;
  std::string RootId() const override { return std::string(); }

  bool Browse(const std::string& id, browse::Listing& out, std::string& error) override;
  // Below a favorite folder the original source searches, if it can; the
  // library's own root has nothing to ask.
  bool CanSearch(const std::string& folder_id) const override;
  bool Search(const std::string& folder_id, const std::string& query,
              browse::Listing& out, std::string& error) override;
  bool Resolve(const browse::Entry& entry, browse::Playable& out, std::string& error) override;

  // Main thread: recomputes the subtitle ("12 items").
  void UpdateDetail();
  // How many entries the root lists right now.
  size_t Count() const;

private:
  bool SearchEverything(const std::string& query, browse::Listing& out, std::string& error);

  Kind kind_;
  Library& library_;
  SourceRegistry& registry_;
  std::string name_;
  std::string detail_;
};

} // namespace library

#endif
