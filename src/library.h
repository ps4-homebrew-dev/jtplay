// SPDX-License-Identifier: GPL-3.0-or-later
//
// The user's own library on top of the browsable sources: favorites (any
// folder or item, from any source) and playback progress (resume points
// and watched marks). Both are remembered across restarts in one small XML
// file in the cache directory.
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
};

// The real sources by key, so a LibrarySource can find the one a ref
// names. The main thread replaces the set after every discovery; the
// worker looks things up.
class SourceRegistry {
public:
  void Set(const std::vector<browse::SourcePtr>& sources);
  browse::SourcePtr Find(const std::string& key) const;

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
  enum class Kind { Favorites, ContinueWatching };

  LibrarySource(Kind kind, Library& library, SourceRegistry& registry);

  const std::string& Name() const override { return name_; }
  const std::string& Detail() const override { return detail_; }
  const char* Icon() const override;
  std::string Key() const override;
  std::string RootId() const override { return std::string(); }

  bool Browse(const std::string& id, browse::Listing& out, std::string& error) override;
  bool Resolve(const browse::Entry& entry, std::string& uri, std::string& error) override;

  // Main thread: recomputes the subtitle ("12 items").
  void UpdateDetail();
  // How many entries the root lists right now.
  size_t Count() const;

private:
  Kind kind_;
  Library& library_;
  SourceRegistry& registry_;
  std::string name_;
  std::string detail_;
};

} // namespace library

#endif
