// SPDX-License-Identifier: GPL-3.0-or-later
//
// The user's own library on top of the browsable sources: favorites (any
// folder or item, from any source), playback progress (resume points,
// where they were played from, and watched marks), what was played lately,
// the sources seen so far (so the list of sources is there before they are
// found again), where each was left and how folders are shown (list or
// grid). All of it is remembered across
// restarts in one small XML file in the cache directory.
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

// A source seen before: enough to list it before it is found again,
// and how often it was opened, for the order of the list.
struct KnownSource {
  std::string key;
  std::string name;
  std::string detail;
  std::string icon;     // a glyph of icons.h
  // Its own picture, a logo (browse::Source::Image), as it said the last
  // time it was found; "" when it has none.
  std::string logo;
  // Artwork seen in it, for the list of sources when it has no picture of
  // its own (see App::NoteSourceArt). Not changed by RememberSource.
  std::string image;
  int uses = 0;
  int64_t used = 0;     // epoch seconds it was last opened, 0 = never
};

// A folder of favorites, made by the user to sort them into; folders go
// inside folders. 'parent' is the folder it is in, "" for the top.
struct FavoriteFolder {
  std::string id;
  std::string name;
  std::string parent;
};

// Something played lately (see App::RecordRecent), which a search of
// everything looks through by name before it asks the sources.
struct Recent {
  Ref ref;
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
  // Adds (at the top, outside any folder) or removes; returns whether it
  // is a favorite now.
  bool ToggleFavorite(const Ref& ref);
  // Adds into 'folder' ("" the top, as is a folder that is not there); one
  // that is a favorite already is moved there.
  void AddFavorite(const Ref& ref, const std::string& folder);
  // Every favorite, in whatever folder, in the order they were added.
  std::vector<Ref> Favorites() const;

  // The folders favorites are sorted into. A favorite is in one folder or
  // none (the top), as is a folder; folders are listed in the order they
  // were made, favorites in the order they were added.
  std::vector<Ref> FavoritesIn(const std::string& folder) const;
  std::vector<FavoriteFolder> FavoriteFolders() const; // all of them
  // How many folders and favorites each folder holds, by its id.
  std::map<std::string, size_t> FavoriteFolderCounts() const;
  bool FindFavoriteFolder(const std::string& id, FavoriteFolder& out) const;
  // The folder a favorite is in, "" for the top (or for no favorite).
  std::string FavoriteFolderOf(const std::string& source_key, const std::string& id) const;
  // Makes a folder in 'parent' ("" the top); returns its id, "" when
  // 'parent' is not there.
  std::string AddFavoriteFolder(const std::string& parent, const std::string& name);
  bool RenameFavoriteFolder(const std::string& id, const std::string& name);
  // Deletes a folder with everything in it: the folders inside it, however
  // deep, and the favorites in any of them.
  bool DeleteFavoriteFolder(const std::string& id);
  // What deleting it would take: the favorites and the folders in it,
  // however deep.
  void FavoriteFolderContents(const std::string& id, size_t& favorites,
                              size_t& folders) const;
  // Into 'folder' ("" the top). A folder cannot go into itself, or into a
  // folder inside it.
  bool MoveFavorite(const std::string& source_key, const std::string& id,
                    const std::string& folder);
  // The name a favorite is listed by (its source's, as it was added, until
  // renamed).
  bool RenameFavorite(const std::string& source_key, const std::string& id,
                      const std::string& name);
  bool MoveFavoriteFolder(const std::string& id, const std::string& parent);
  // The folders favorites were saved or moved to lately, the last first
  // ("" the top), for saving the next one there with a press.
  void NoteFavoriteFolderUsed(const std::string& folder);
  std::vector<std::string> RecentFavoriteFolders() const;
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
  // Where an item with a resume point was played from, so that resuming
  // it can open that folder again (and play on through it from there).
  void SetResumePlace(const std::string& source_key, const std::string& id,
                      const std::string& place_source, const std::vector<PlaceLevel>& place);
  bool ResumePlace(const std::string& source_key, const std::string& id,
                   std::string& place_source, std::vector<PlaceLevel>& place) const;
  // Unfinished items, most recently played first.
  std::vector<std::pair<Ref, Progress>> InProgress() const;

  // Lately played (see Recent), most recent first. Adding one already
  // there moves it to the front.
  void AddRecent(const Ref& ref);
  std::vector<Recent> RecentItems() const;

  // The sources seen: most opened first, then the order they were found
  // in. Remember adds one or updates what it says about itself; Used
  // counts an opening.
  void RememberSource(const KnownSource& source);
  void SourceUsed(const std::string& key);
  // Artwork for a source that has none yet; true if taken.
  bool SetSourceImage(const std::string& key, const std::string& image);
  void ForgetSource(const std::string& key);
  std::vector<KnownSource> KnownSources() const;
  bool FindKnownSource(const std::string& key, KnownSource& out) const;

  // Where each source was left, to open it there again; an empty list
  // forgets it. Not a change of the library as views see it (no new
  // generation): nothing on screen shows it.
  void SetPlace(const std::string& source_key, const std::vector<PlaceLevel>& levels);
  std::vector<PlaceLevel> Place(const std::string& source_key) const;

  // How a folder is shown, if it was ever set for it: as a grid, or a
  // list. False when not (the app then goes by the folder above it). Not
  // a change views need to know of either.
  void SetView(const std::string& source_key, const std::string& id, bool grid);
  bool GetView(const std::string& source_key, const std::string& id, bool& grid) const;

  // Bumped on every change, so views can tell when to refresh.
  uint64_t Generation() const;

private:
  struct Item {
    Ref ref;
    Progress progress;
    // Where it was played from (see SetResumePlace): a source's key and
    // the folders down to the one it is in, on it.
    std::string place_source;
    std::vector<PlaceLevel> place;
  };
  static std::string Id(const std::string& key, const std::string& id) {
    return key + '\n' + id;
  }
  void Changed(); // with mutex_ held

  mutable std::mutex mutex_;
  std::string path_;
  bool dirty_ = false;
  uint64_t generation_ = 0;
  struct Favorite {
    Ref ref;
    std::string folder;                 // FavoriteFolder::id, "" = the top
  };
  bool HasFolder(const std::string& id) const;    // "" (the top) too
  bool Inside(std::string id, const std::string& ancestor) const;
  std::vector<Favorite> favorites_;     // in the order they were added
  std::vector<FavoriteFolder> folders_; // in the order they were made
  uint64_t next_folder_ = 1;            // for the next folder's id
  std::vector<std::string> recent_folders_; // see RecentFavoriteFolders
  std::map<std::string, Item> progress_;
  std::vector<Recent> recent_;          // most recent first
  std::vector<KnownSource> sources_;    // in the order they were found
  std::map<std::string, std::vector<PlaceLevel>> places_; // by source key
  struct View {
    bool grid = false;
    int64_t updated = 0;  // epoch seconds, to forget the oldest
  };
  std::map<std::string, View> views_;   // by Id()
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

// Under Favorites, a folder of them (FavoriteFolder) is listed under an id
// of its own ("fav:<id>"), which no entry of a source has there (theirs are
// encoded, above).
std::string FavoriteFolderEntryId(const std::string& folder);
bool FavoriteFolderFromEntryId(const std::string& entry_id, std::string& folder);

class LibrarySource : public browse::Source {
public:
  // Sources: the list of every source (the app makes its root listing,
  // see App::OpenSourcesList); searched there, it searches the library (by
  // name) and every source that can search, and lists what they found.
  enum class Kind { Favorites, ContinueWatching, Sources };

  LibrarySource(Kind kind, Library& library, SourceRegistry& registry);

  const std::string& Name() const override { return name_; }
  const std::string& Detail() const override { return detail_; }
  const char* Icon() const override;
  std::string Key() const override;
  std::string RootId() const override { return std::string(); }

  bool Browse(const std::string& id, browse::Listing& out, std::string& error) override;
  // Below a favorite folder, the original source's pages (the roots are
  // listed whole).
  bool BrowsePage(const std::string& id, const std::string& cursor, browse::Listing& out,
                  std::string& next, std::string& error) override;
  // Below a favorite folder the original source searches, if it can; of
  // the roots, only that of Sources can (everything, see above).
  bool CanSearch(const std::string& folder_id) const override;
  bool Search(const std::string& folder_id, const std::string& query,
              browse::Listing& out, std::string& error) override;
  bool SearchPage(const std::string& folder_id, const std::string& query,
                  const std::string& cursor, browse::Listing& out, std::string& next,
                  std::string& error) override;
  bool Resolve(const browse::Entry& entry, browse::Playable& out, std::string& error) override;

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
