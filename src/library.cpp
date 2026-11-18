// SPDX-License-Identifier: GPL-3.0-or-later
#include "library.h"
#include "icons.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <thread>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <set>

#include "net/cancel.h"

#include <tinyxml2.h>

namespace library {

namespace {

using tinyxml2::XMLDocument;
using tinyxml2::XMLElement;

// Markers around the source key in an encoded id; neither appears in a
// path, a URL or a DIDL object id.
constexpr char kIdStart = '\x01';
constexpr char kIdSep = '\x02';

constexpr size_t kMaxProgress = 1000;   // remembered items, oldest dropped
constexpr size_t kMaxInProgress = 50;   // shown under "Continue watching"
constexpr size_t kMaxRecent = 30;       // remembered lately played
constexpr size_t kMaxSources = 200;     // remembered sources
constexpr size_t kMaxPlaceDepth = 32;   // folders remembered per source
constexpr size_t kMaxViews = 500;       // folders whose view was set

std::string Lower(std::string s) {
  for (char& c : s)
    c = (char)std::tolower((unsigned char)c);
  return s;
}

std::string Attr(const XMLElement* e, const char* name) {
  const char* v = e->Attribute(name);
  return v ? v : "";
}

void WriteRef(XMLElement* e, const Ref& ref) {
  const browse::Entry& en = ref.entry;
  e->SetAttribute("source", ref.source_key.c_str());
  e->SetAttribute("source_name", ref.source_name.c_str());
  e->SetAttribute("id", en.id.c_str());
  e->SetAttribute("type", browse::TypeName(en.type));
  e->SetAttribute("name", en.name.c_str());
  if (!en.description.empty()) e->SetAttribute("description", en.description.c_str());
  if (!en.image.empty())       e->SetAttribute("image", en.image.c_str());
  if (!en.uri.empty())         e->SetAttribute("uri", en.uri.c_str());
  if (en.size >= 0)            e->SetAttribute("size", en.size);
  if (en.date > 0)             e->SetAttribute("date", en.date);
  if (en.duration > 0)         e->SetAttribute("duration", en.duration);
}

bool ReadRef(const XMLElement* e, Ref& ref) {
  ref.source_key = Attr(e, "source");
  ref.source_name = Attr(e, "source_name");
  browse::Entry& en = ref.entry;
  en.id = Attr(e, "id");
  en.type = browse::TypeFromName(Attr(e, "type"));
  en.name = Attr(e, "name");
  en.description = Attr(e, "description");
  en.image = Attr(e, "image");
  en.uri = Attr(e, "uri");
  en.size = e->Int64Attribute("size", -1);
  en.date = e->Int64Attribute("date", 0);
  en.duration = e->Int64Attribute("duration", 0);
  return !ref.source_key.empty() && !en.id.empty();
}

} // namespace

// ---------------------------------------------------------------------------
// Library
// ---------------------------------------------------------------------------

void Library::Load(const std::string& path) {
  std::lock_guard<std::mutex> lock(mutex_);
  path_ = path;
  favorites_.clear();
  folders_.clear();
  recent_folders_.clear();
  next_folder_ = 1;
  progress_.clear();
  recent_.clear();
  sources_.clear();
  places_.clear();
  views_.clear();

  XMLDocument doc;
  if (doc.LoadFile(path.c_str()) != tinyxml2::XML_SUCCESS || !doc.RootElement())
    return; // first run, or unreadable: start empty

  for (const XMLElement* e = doc.RootElement()->FirstChildElement(); e;
       e = e->NextSiblingElement()) {
    if (std::string(e->Name()) == "place") {
      std::vector<PlaceLevel> levels;
      for (const XMLElement* l = e->FirstChildElement("level"); l;
           l = l->NextSiblingElement("level"))
        levels.push_back({Attr(l, "id"), Attr(l, "name"), Attr(l, "selected")});
      if (!Attr(e, "source").empty() && !levels.empty())
        places_[Attr(e, "source")] = std::move(levels);
      continue;
    }
    if (std::string(e->Name()) == "view") {
      View v;
      v.grid = e->BoolAttribute("grid", false);
      v.updated = e->Int64Attribute("updated", 0);
      if (!Attr(e, "source").empty())
        views_[Id(Attr(e, "source"), Attr(e, "id"))] = v;
      continue;
    }
    if (std::string(e->Name()) == "recentfolder") {
      recent_folders_.push_back(Attr(e, "id"));
      continue;
    }
    if (std::string(e->Name()) == "folder") {
      FavoriteFolder f;
      f.id = Attr(e, "id");
      f.name = Attr(e, "name");
      f.parent = Attr(e, "parent");
      if (!f.id.empty() && !HasFolder(f.id))
        folders_.push_back(std::move(f));
      continue;
    }
    if (std::string(e->Name()) == "source") {
      KnownSource s;
      s.key = Attr(e, "key");
      s.name = Attr(e, "name");
      s.detail = Attr(e, "detail");
      s.icon = Attr(e, "icon");
      s.logo = Attr(e, "logo");
      s.image = Attr(e, "image");
      s.uses = e->IntAttribute("uses", 0);
      s.used = e->Int64Attribute("used", 0);
      if (!s.key.empty() && !s.name.empty())
        sources_.push_back(std::move(s));
      continue;
    }
    Ref ref;
    if (!ReadRef(e, ref))
      continue;
    if (std::string(e->Name()) == "favorite") {
      favorites_.push_back({std::move(ref), Attr(e, "folder")});
    } else if (std::string(e->Name()) == "recent") {
      Recent r;
      r.updated = e->Int64Attribute("updated", 0);
      r.ref = std::move(ref);
      recent_.push_back(std::move(r));
    } else if (std::string(e->Name()) == "progress") {
      Item item;
      item.progress.position = e->Int64Attribute("position", 0);
      item.progress.duration = e->Int64Attribute("total", 0);
      item.progress.watched = e->BoolAttribute("watched", false);
      item.progress.updated = e->Int64Attribute("updated", 0);
      item.place_source = Attr(e, "place_source");
      for (const XMLElement* l = e->FirstChildElement("level"); l;
           l = l->NextSiblingElement("level"))
        item.place.push_back({Attr(l, "id"), Attr(l, "name"), Attr(l, "selected")});
      item.ref = std::move(ref);
      const std::string id = Id(item.ref.source_key, item.ref.entry.id);
      progress_[id] = std::move(item);
    }
  }

  // Whatever is in a folder that is not there (or in a circle of folders,
  // which an edited file could make) goes to the top.
  for (FavoriteFolder& f : folders_)
    if (!HasFolder(f.parent) || Inside(f.parent, f.id))
      f.parent.clear();
  for (Favorite& f : favorites_)
    if (!HasFolder(f.folder))
      f.folder.clear();
  recent_folders_.erase(std::remove_if(recent_folders_.begin(), recent_folders_.end(),
                                       [this](const std::string& id) { return !HasFolder(id); }),
                        recent_folders_.end());
  for (const FavoriteFolder& f : folders_)
    next_folder_ = std::max<uint64_t>(next_folder_,
                                      std::strtoull(f.id.c_str(), nullptr, 10) + 1);
  generation_++;
}

void Library::Save() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!dirty_ || path_.empty())
    return;

  XMLDocument doc;
  doc.InsertEndChild(doc.NewDeclaration());
  XMLElement* root = doc.NewElement("library");
  root->SetAttribute("version", 1);
  doc.InsertEndChild(root);

  for (const FavoriteFolder& f : folders_) {
    XMLElement* e = doc.NewElement("folder");
    e->SetAttribute("id", f.id.c_str());
    e->SetAttribute("name", f.name.c_str());
    if (!f.parent.empty())
      e->SetAttribute("parent", f.parent.c_str());
    root->InsertEndChild(e);
  }
  for (const std::string& id : recent_folders_) {
    XMLElement* e = doc.NewElement("recentfolder");
    e->SetAttribute("id", id.c_str());
    root->InsertEndChild(e);
  }
  for (const Favorite& f : favorites_) {
    XMLElement* e = doc.NewElement("favorite");
    WriteRef(e, f.ref);
    if (!f.folder.empty())
      e->SetAttribute("folder", f.folder.c_str());
    root->InsertEndChild(e);
  }
  for (const Recent& r : recent_) {
    XMLElement* e = doc.NewElement("recent");
    WriteRef(e, r.ref);
    e->SetAttribute("updated", r.updated);
    root->InsertEndChild(e);
  }
  for (const KnownSource& s : sources_) {
    XMLElement* e = doc.NewElement("source");
    e->SetAttribute("key", s.key.c_str());
    e->SetAttribute("name", s.name.c_str());
    if (!s.detail.empty()) e->SetAttribute("detail", s.detail.c_str());
    if (!s.icon.empty())   e->SetAttribute("icon", s.icon.c_str());
    if (!s.logo.empty())   e->SetAttribute("logo", s.logo.c_str());
    if (!s.image.empty())  e->SetAttribute("image", s.image.c_str());
    if (s.uses > 0)        e->SetAttribute("uses", s.uses);
    if (s.used > 0)        e->SetAttribute("used", s.used);
    root->InsertEndChild(e);
  }
  for (const auto& kv : places_) {
    XMLElement* e = doc.NewElement("place");
    e->SetAttribute("source", kv.first.c_str());
    for (const PlaceLevel& level : kv.second) {
      XMLElement* l = doc.NewElement("level");
      l->SetAttribute("id", level.id.c_str());
      l->SetAttribute("name", level.name.c_str());
      if (!level.selected.empty())
        l->SetAttribute("selected", level.selected.c_str());
      e->InsertEndChild(l);
    }
    root->InsertEndChild(e);
  }
  for (const auto& kv : views_) {
    const size_t sep = kv.first.find('\n');
    XMLElement* e = doc.NewElement("view");
    e->SetAttribute("source", kv.first.substr(0, sep).c_str());
    e->SetAttribute("id", kv.first.substr(sep + 1).c_str());
    e->SetAttribute("grid", kv.second.grid);
    e->SetAttribute("updated", kv.second.updated);
    root->InsertEndChild(e);
  }
  for (const auto& kv : progress_) {
    const Item& item = kv.second;
    XMLElement* e = doc.NewElement("progress");
    WriteRef(e, item.ref);
    e->SetAttribute("position", item.progress.position);
    e->SetAttribute("total", item.progress.duration);
    e->SetAttribute("watched", item.progress.watched);
    e->SetAttribute("updated", item.progress.updated);
    if (!item.place_source.empty() && !item.place.empty()) {
      e->SetAttribute("place_source", item.place_source.c_str());
      for (const PlaceLevel& level : item.place) {
        XMLElement* l = doc.NewElement("level");
        l->SetAttribute("id", level.id.c_str());
        l->SetAttribute("name", level.name.c_str());
        if (!level.selected.empty())
          l->SetAttribute("selected", level.selected.c_str());
        e->InsertEndChild(l);
      }
    }
    root->InsertEndChild(e);
  }

  // Write next to it and rename, so a crash mid-write never loses the lot.
  const std::string tmp = path_ + ".tmp";
  if (doc.SaveFile(tmp.c_str()) == tinyxml2::XML_SUCCESS &&
      std::rename(tmp.c_str(), path_.c_str()) == 0)
    dirty_ = false;
}

void Library::Changed() {
  dirty_ = true;
  generation_++;
}

uint64_t Library::Generation() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return generation_;
}

bool Library::IsFavorite(const std::string& key, const std::string& id) const {
  std::lock_guard<std::mutex> lock(mutex_);
  for (const Favorite& f : favorites_)
    if (f.ref.source_key == key && f.ref.entry.id == id)
      return true;
  return false;
}

bool Library::ToggleFavorite(const Ref& ref) {
  std::lock_guard<std::mutex> lock(mutex_);
  for (auto it = favorites_.begin(); it != favorites_.end(); ++it) {
    if (it->ref.source_key == ref.source_key && it->ref.entry.id == ref.entry.id) {
      favorites_.erase(it);
      Changed();
      return false;
    }
  }
  favorites_.push_back({ref, std::string()});
  Changed();
  return true;
}

void Library::AddFavorite(const Ref& ref, const std::string& folder) {
  std::lock_guard<std::mutex> lock(mutex_);
  const std::string into = HasFolder(folder) ? folder : std::string();
  for (Favorite& f : favorites_) {
    if (f.ref.source_key == ref.source_key && f.ref.entry.id == ref.entry.id) {
      f.folder = into;
      Changed();
      return;
    }
  }
  favorites_.push_back({ref, into});
  Changed();
}

bool Library::HasFolder(const std::string& id) const {
  if (id.empty())
    return true;
  for (const FavoriteFolder& f : folders_)
    if (f.id == id)
      return true;
  return false;
}

// Whether folder 'id' is 'ancestor' or inside it, however deep. (A chain
// longer than there are folders is a circle: inside, so it is not made
// worse.)
bool Library::Inside(std::string id, const std::string& ancestor) const {
  for (size_t steps = 0; !id.empty(); steps++) {
    if (id == ancestor || steps > folders_.size())
      return true;
    std::string parent;
    for (const FavoriteFolder& f : folders_)
      if (f.id == id)
        parent = f.parent;
    id = parent;
  }
  return false;
}

std::vector<Ref> Library::FavoritesIn(const std::string& folder) const {
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<Ref> out;
  for (const Favorite& f : favorites_)
    if (f.folder == folder)
      out.push_back(f.ref);
  return out;
}

std::vector<FavoriteFolder> Library::FavoriteFolders() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return folders_;
}

std::map<std::string, size_t> Library::FavoriteFolderCounts() const {
  std::lock_guard<std::mutex> lock(mutex_);
  std::map<std::string, size_t> out;
  for (const FavoriteFolder& f : folders_)
    out[f.parent]++;
  for (const Favorite& f : favorites_)
    out[f.folder]++;
  return out;
}

bool Library::FindFavoriteFolder(const std::string& id, FavoriteFolder& out) const {
  std::lock_guard<std::mutex> lock(mutex_);
  for (const FavoriteFolder& f : folders_) {
    if (f.id == id) {
      out = f;
      return true;
    }
  }
  return false;
}

std::string Library::FavoriteFolderOf(const std::string& key, const std::string& id) const {
  std::lock_guard<std::mutex> lock(mutex_);
  for (const Favorite& f : favorites_)
    if (f.ref.source_key == key && f.ref.entry.id == id)
      return f.folder;
  return {};
}

std::string Library::AddFavoriteFolder(const std::string& parent, const std::string& name) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!HasFolder(parent) || name.empty())
    return {};
  FavoriteFolder f;
  f.id = std::to_string(next_folder_++);
  f.name = name;
  f.parent = parent;
  folders_.push_back(f);
  Changed();
  return f.id;
}

bool Library::RenameFavoriteFolder(const std::string& id, const std::string& name) {
  std::lock_guard<std::mutex> lock(mutex_);
  for (FavoriteFolder& f : folders_) {
    if (f.id == id && !name.empty()) {
      if (f.name != name) {
        f.name = name;
        Changed();
      }
      return true;
    }
  }
  return false;
}

bool Library::DeleteFavoriteFolder(const std::string& id) {
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = std::find_if(folders_.begin(), folders_.end(),
                         [&id](const FavoriteFolder& f) { return f.id == id; });
  if (it == folders_.end())
    return false;
  // It and everything inside it.
  std::set<std::string> gone;
  for (const FavoriteFolder& f : folders_)
    if (Inside(f.id, id))
      gone.insert(f.id);
  folders_.erase(std::remove_if(folders_.begin(), folders_.end(),
                                [&gone](const FavoriteFolder& f) { return gone.count(f.id); }),
                 folders_.end());
  favorites_.erase(std::remove_if(favorites_.begin(), favorites_.end(),
                                  [&gone](const Favorite& f) { return gone.count(f.folder); }),
                   favorites_.end());
  recent_folders_.erase(std::remove_if(recent_folders_.begin(), recent_folders_.end(),
                                       [&gone](const std::string& f) { return gone.count(f); }),
                        recent_folders_.end());
  Changed();
  return true;
}

void Library::FavoriteFolderContents(const std::string& id, size_t& favorites,
                                     size_t& folders) const {
  std::lock_guard<std::mutex> lock(mutex_);
  favorites = folders = 0;
  if (id.empty())
    return;
  std::set<std::string> in;
  for (const FavoriteFolder& f : folders_)
    if (Inside(f.id, id))
      in.insert(f.id);
  folders = in.empty() ? 0 : in.size() - 1; // (not itself)
  for (const Favorite& f : favorites_)
    favorites += in.count(f.folder);
}

void Library::NoteFavoriteFolderUsed(const std::string& folder) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!HasFolder(folder))
    return;
  recent_folders_.erase(std::remove(recent_folders_.begin(), recent_folders_.end(), folder),
                        recent_folders_.end());
  recent_folders_.insert(recent_folders_.begin(), folder);
  if (recent_folders_.size() > 8)
    recent_folders_.resize(8);
  dirty_ = true; // (nothing on screen shows it: no new generation)
}

std::vector<std::string> Library::RecentFavoriteFolders() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return recent_folders_;
}

bool Library::MoveFavorite(const std::string& key, const std::string& id,
                           const std::string& folder) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!HasFolder(folder))
    return false;
  for (Favorite& f : favorites_) {
    if (f.ref.source_key == key && f.ref.entry.id == id) {
      if (f.folder != folder) {
        f.folder = folder;
        Changed();
      }
      return true;
    }
  }
  return false;
}

bool Library::MoveFavoriteFolder(const std::string& id, const std::string& parent) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!HasFolder(parent) || Inside(parent, id))
    return false;
  for (FavoriteFolder& f : folders_) {
    if (f.id == id) {
      if (f.parent != parent) {
        f.parent = parent;
        Changed();
      }
      return true;
    }
  }
  return false;
}

std::string Library::SourceName(const std::string& source_key) const {
  std::lock_guard<std::mutex> lock(mutex_);
  for (const KnownSource& s : sources_)
    if (s.key == source_key)
      return s.name;
  for (const Favorite& f : favorites_)
    if (f.ref.source_key == source_key && !f.ref.source_name.empty())
      return f.ref.source_name;
  for (const auto& item : progress_)
    if (item.second.ref.source_key == source_key && !item.second.ref.source_name.empty())
      return item.second.ref.source_name;
  return {};
}

std::vector<Ref> Library::Favorites() const {
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<Ref> out;
  for (const Favorite& f : favorites_)
    out.push_back(f.ref);
  return out;
}

bool Library::GetProgress(const std::string& key, const std::string& id,
                          Progress& out) const {
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = progress_.find(Id(key, id));
  if (it == progress_.end())
    return false;
  out = it->second.progress;
  return true;
}

void Library::SetResume(const Ref& ref, int64_t position, int64_t duration) {
  std::lock_guard<std::mutex> lock(mutex_);
  Item& item = progress_[Id(ref.source_key, ref.entry.id)];
  item.ref = ref;
  item.progress.position = position;
  item.progress.duration = duration;
  item.progress.watched = false;
  item.progress.updated = (int64_t)std::time(nullptr);

  // Keep the file small: forget the items played longest ago.
  if (progress_.size() > kMaxProgress) {
    auto oldest = progress_.begin();
    for (auto it = progress_.begin(); it != progress_.end(); ++it)
      if (it->second.progress.updated < oldest->second.progress.updated)
        oldest = it;
    progress_.erase(oldest);
  }
  Changed();
}

void Library::SetWatched(const Ref& ref, bool watched) {
  std::lock_guard<std::mutex> lock(mutex_);
  const std::string id = Id(ref.source_key, ref.entry.id);
  if (!watched) {
    if (progress_.erase(id))
      Changed();
    return;
  }
  Item& item = progress_[id];
  item.ref = ref;
  item.progress.position = 0;
  item.progress.watched = true;
  item.progress.updated = (int64_t)std::time(nullptr);
  Changed();
}

void Library::SetResumePlace(const std::string& key, const std::string& id,
                             const std::string& place_source,
                             const std::vector<PlaceLevel>& place) {
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = progress_.find(Id(key, id));
  if (it == progress_.end())
    return;
  std::vector<PlaceLevel> kept(place.begin(),
                               place.begin() + std::min(place.size(), kMaxPlaceDepth));
  it->second.place_source = place_source;
  it->second.place = std::move(kept);
  dirty_ = true; // saved with the rest; nothing on screen shows it
}

bool Library::ResumePlace(const std::string& key, const std::string& id,
                          std::string& place_source, std::vector<PlaceLevel>& place) const {
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = progress_.find(Id(key, id));
  if (it == progress_.end() || it->second.place_source.empty() || it->second.place.empty())
    return false;
  place_source = it->second.place_source;
  place = it->second.place;
  return true;
}

std::string Library::ImageFor(const std::string& key, const std::string& id) const {
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = progress_.find(Id(key, id));
  return it == progress_.end() ? std::string() : it->second.ref.entry.image;
}

std::vector<std::pair<Ref, Progress>> Library::InProgress() const {
  std::vector<std::pair<Ref, Progress>> out;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& kv : progress_) {
      if (!kv.second.progress.watched && kv.second.progress.position > 0)
        out.emplace_back(kv.second.ref, kv.second.progress);
    }
  }
  std::sort(out.begin(), out.end(), [](const auto& a, const auto& b) {
    return a.second.updated > b.second.updated;
  });
  if (out.size() > kMaxInProgress)
    out.resize(kMaxInProgress);
  return out;
}

void Library::AddRecent(const Ref& ref) {
  std::lock_guard<std::mutex> lock(mutex_);
  recent_.erase(std::remove_if(recent_.begin(), recent_.end(), [&ref](const Recent& r) {
    return r.ref.source_key == ref.source_key && r.ref.entry.id == ref.entry.id;
  }), recent_.end());
  Recent r;
  r.ref = ref;
  r.updated = (int64_t)std::time(nullptr);
  recent_.insert(recent_.begin(), std::move(r));
  if (recent_.size() > kMaxRecent)
    recent_.resize(kMaxRecent);
  Changed();
}

std::vector<Recent> Library::RecentItems() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return recent_;
}

void Library::RememberSource(const KnownSource& source) {
  std::lock_guard<std::mutex> lock(mutex_);
  for (KnownSource& s : sources_) {
    if (s.key != source.key)
      continue;
    // What it says about itself may have changed (a server renamed, a
    // drive's free space); how often it was opened has not.
    if (s.name != source.name || s.detail != source.detail || s.icon != source.icon ||
        s.logo != source.logo) {
      s.name = source.name;
      s.detail = source.detail;
      s.icon = source.icon;
      s.logo = source.logo;
      Changed();
    }
    return;
  }
  KnownSource s = source;
  s.uses = 0;
  s.used = 0;
  s.image.clear();
  sources_.push_back(std::move(s));
  if (sources_.size() > kMaxSources) {
    // The one opened least (and longest ago) makes way.
    auto least = std::min_element(sources_.begin(), sources_.end(),
      [](const KnownSource& a, const KnownSource& b) {
        return a.uses != b.uses ? a.uses < b.uses : a.used < b.used;
      });
    sources_.erase(least);
  }
  Changed();
}

void Library::SourceUsed(const std::string& key) {
  std::lock_guard<std::mutex> lock(mutex_);
  for (KnownSource& s : sources_) {
    if (s.key == key) {
      s.uses++;
      s.used = (int64_t)std::time(nullptr);
      Changed();
      return;
    }
  }
}

bool Library::SetSourceImage(const std::string& key, const std::string& image) {
  std::lock_guard<std::mutex> lock(mutex_);
  for (KnownSource& s : sources_) {
    if (s.key != key)
      continue;
    if (!s.image.empty() || image.empty())
      return false;
    s.image = image;
    Changed();
    return true;
  }
  return false;
}

void Library::ForgetSource(const std::string& key) {
  std::lock_guard<std::mutex> lock(mutex_);
  const size_t before = sources_.size();
  sources_.erase(std::remove_if(sources_.begin(), sources_.end(),
                                [&key](const KnownSource& s) { return s.key == key; }),
                 sources_.end());
  if (sources_.size() != before)
    Changed();
}

bool Library::FindKnownSource(const std::string& key, KnownSource& out) const {
  std::lock_guard<std::mutex> lock(mutex_);
  for (const KnownSource& s : sources_) {
    if (s.key == key) {
      out = s;
      return true;
    }
  }
  return false;
}

std::vector<KnownSource> Library::KnownSources() const {
  std::vector<KnownSource> out;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    out = sources_;
  }
  // Most opened first; among those opened as often, the one opened last.
  // Never opened: the order they were found in.
  std::stable_sort(out.begin(), out.end(), [](const KnownSource& a, const KnownSource& b) {
    if (a.uses != b.uses)
      return a.uses > b.uses;
    return a.used > b.used;
  });
  return out;
}

void Library::SetPlace(const std::string& key, const std::vector<PlaceLevel>& levels) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (levels.empty()) {
    if (places_.erase(key))
      dirty_ = true;
    return;
  }
  std::vector<PlaceLevel> kept(levels.begin(),
                               levels.begin() + std::min(levels.size(), kMaxPlaceDepth));
  auto same = [](const std::vector<PlaceLevel>& a, const std::vector<PlaceLevel>& b) {
    if (a.size() != b.size())
      return false;
    for (size_t i = 0; i < a.size(); i++)
      if (a[i].id != b[i].id || a[i].selected != b[i].selected)
        return false;
    return true;
  };
  auto it = places_.find(key);
  if (it != places_.end() && same(it->second, kept))
    return;
  places_[key] = std::move(kept);
  // Only for sources still remembered: a place without one would linger.
  for (auto p = places_.begin(); p != places_.end();) {
    const bool known = std::any_of(sources_.begin(), sources_.end(),
      [&p](const KnownSource& s) { return s.key == p->first; });
    p = known ? std::next(p) : places_.erase(p);
  }
  dirty_ = true; // saved with the rest; views need not know
}

std::vector<PlaceLevel> Library::Place(const std::string& key) const {
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = places_.find(key);
  return it == places_.end() ? std::vector<PlaceLevel>() : it->second;
}

void Library::SetView(const std::string& key, const std::string& id, bool grid) {
  std::lock_guard<std::mutex> lock(mutex_);
  View& v = views_[Id(key, id)];
  v.grid = grid;
  v.updated = (int64_t)std::time(nullptr);
  // Keep the file small: forget the folders set longest ago.
  if (views_.size() > kMaxViews) {
    auto oldest = views_.begin();
    for (auto it = views_.begin(); it != views_.end(); ++it)
      if (it->second.updated < oldest->second.updated)
        oldest = it;
    views_.erase(oldest);
  }
  dirty_ = true; // saved with the rest; views need not know
}

bool Library::GetView(const std::string& key, const std::string& id, bool& grid) const {
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = views_.find(Id(key, id));
  if (it == views_.end())
    return false;
  grid = it->second.grid;
  return true;
}

// ---------------------------------------------------------------------------
// SourceRegistry
// ---------------------------------------------------------------------------

void SourceRegistry::Set(const std::vector<browse::SourcePtr>& sources) {
  std::map<std::string, browse::SourcePtr> map;
  for (const browse::SourcePtr& s : sources)
    map.emplace(s->Key(), s);
  std::lock_guard<std::mutex> lock(mutex_);
  sources_ = std::move(map);
}

std::vector<browse::SourcePtr> SourceRegistry::All() const {
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<browse::SourcePtr> out;
  for (const auto& kv : sources_)
    out.push_back(kv.second);
  return out;
}

browse::SourcePtr SourceRegistry::Find(const std::string& key) const {
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = sources_.find(key);
  return it == sources_.end() ? nullptr : it->second;
}

// ---------------------------------------------------------------------------
// Ids
// ---------------------------------------------------------------------------

std::string EncodeId(const std::string& key, const std::string& id) {
  return std::string(1, kIdStart) + key + kIdSep + id;
}

bool DecodeId(const std::string& encoded, std::string& key, std::string& id) {
  if (encoded.empty() || encoded[0] != kIdStart)
    return false;
  const size_t sep = encoded.find(kIdSep, 1);
  if (sep == std::string::npos)
    return false;
  key = encoded.substr(1, sep - 1);
  id = encoded.substr(sep + 1);
  return true;
}

// ---------------------------------------------------------------------------
// LibrarySource
// ---------------------------------------------------------------------------

LibrarySource::LibrarySource(Kind kind, Library& library, SourceRegistry& registry)
  : kind_(kind), library_(library), registry_(registry),
    name_(kind == Kind::Favorites ? "Favorites"
          : kind == Kind::Sources ? "Sources"
                                  : "Continue watching"),
    detail_(kind == Kind::Favorites ? "What you pinned"
            : kind == Kind::Sources ? "Drives, media servers and plugins"
                                    : "What you left unfinished") {}

const char* LibrarySource::Icon() const {
  return kind_ == Kind::Favorites ? icon::kStar
       : kind_ == Kind::Sources   ? icon::kDrive
                                  : icon::kResume;
}

std::string LibrarySource::Key() const {
  return kind_ == Kind::Favorites ? "library:favorites"
       : kind_ == Kind::Sources   ? "library:sources"
                                  : "library:continue";
}

std::string FavoriteFolderEntryId(const std::string& folder) {
  return "fav:" + folder;
}

bool FavoriteFolderFromEntryId(const std::string& entry_id, std::string& folder) {
  if (entry_id.compare(0, 4, "fav:") != 0)
    return false;
  folder = entry_id.substr(4);
  return true;
}

bool LibrarySource::Browse(const std::string& id, browse::Listing& out,
                           std::string& error) {
  // Favorites: the folders in the one asked for (the top, or one made to
  // sort them into), in the order they were made, then its favorites, the
  // last added first.
  std::string folder;
  if (kind_ == Kind::Favorites && (id.empty() || FavoriteFolderFromEntryId(id, folder))) {
    if (!folder.empty()) {
      FavoriteFolder f;
      if (!library_.FindFavoriteFolder(folder, f)) {
        error = "this folder is not there any more";
        return false;
      }
    }
    const std::map<std::string, size_t> counts = library_.FavoriteFolderCounts();
    for (const FavoriteFolder& f : library_.FavoriteFolders()) {
      if (f.parent != folder)
        continue;
      const auto it = counts.find(f.id);
      const size_t count = it == counts.end() ? 0 : it->second;
      browse::Entry e;
      e.id = FavoriteFolderEntryId(f.id);
      e.type = browse::Entry::Type::Folder;
      e.name = f.name;
      e.description = count == 0 ? "Empty"
        : std::to_string(count) + (count == 1 ? " item" : " items");
      out.push_back(std::move(e));
    }
    std::vector<Ref> refs = library_.FavoritesIn(folder);
    for (auto it = refs.rbegin(); it != refs.rend(); ++it) {
      browse::Entry e = it->entry;
      e.id = EncodeId(it->source_key, it->entry.id);
      out.push_back(std::move(e));
    }
    return true;
  }

  if (id.empty()) {
    // The root: the stored refs, under ids that remember their source.
    // The progress and the source name are the app's to show (it looks
    // both up by the decoded id), so the entries go out as stored.
    auto add = [&out](const Ref& ref) {
      browse::Entry e = ref.entry;
      e.id = EncodeId(ref.source_key, ref.entry.id);
      out.push_back(std::move(e));
    };
    if (kind_ == Kind::Sources) {
      // (Made by the app, which knows which are found: App::OpenSourcesList.)
    } else {
      for (const auto& item : library_.InProgress())
        add(item.first);
    }
    return true;
  }

  // Below a favorite folder: the original source does the work, and its
  // ids are wrapped again on the way out.
  std::string key, inner;
  if (!DecodeId(id, key, inner)) {
    error = "unknown library entry";
    return false;
  }
  const browse::SourcePtr source = registry_.Find(key);
  if (!source) {
    error = "that source is not available right now";
    return false;
  }
  if (!source->Browse(inner, out, error))
    return false;
  for (browse::Entry& e : out)
    e.id = EncodeId(key, e.id);
  return true;
}

bool LibrarySource::BrowsePage(const std::string& id, const std::string& cursor,
                               browse::Listing& out, std::string& next, std::string& error) {
  std::string key, inner;
  if (id.empty() || !DecodeId(id, key, inner))
    return browse::Source::BrowsePage(id, cursor, out, next, error);
  const browse::SourcePtr source = registry_.Find(key);
  if (!source) {
    error = "that source is not available right now";
    return false;
  }
  if (!source->BrowsePage(inner, cursor, out, next, error))
    return false;
  for (browse::Entry& e : out)
    e.id = EncodeId(key, e.id);
  return true;
}

bool LibrarySource::CanSearch(const std::string& folder_id) const {
  std::string key, inner;
  if (!DecodeId(folder_id, key, inner))
    return kind_ == Kind::Sources; // the root: of everything, or of the library
  const browse::SourcePtr source = registry_.Find(key);
  return source && source->CanSearch(inner);
}

bool LibrarySource::Search(const std::string& folder_id, const std::string& query,
                           browse::Listing& out, std::string& error) {
  std::string key, inner;
  if (!DecodeId(folder_id, key, inner)) {
    if (kind_ == Kind::Sources)
      return SearchEverything(query, out, error);
    error = "this source cannot search";
    return false;
  }
  const browse::SourcePtr source = registry_.Find(key);
  if (!source) {
    error = "that source is not available right now";
    return false;
  }
  if (!source->Search(inner, query, out, error))
    return false;
  for (browse::Entry& e : out)
    e.id = EncodeId(key, e.id);
  return true;
}

bool LibrarySource::SearchPage(const std::string& folder_id, const std::string& query,
                               const std::string& cursor, browse::Listing& out,
                               std::string& next, std::string& error) {
  std::string key, inner;
  if (!DecodeId(folder_id, key, inner))
    return browse::Source::SearchPage(folder_id, query, cursor, out, next, error);
  const browse::SourcePtr source = registry_.Find(key);
  if (!source) {
    error = "that source is not available right now";
    return false;
  }
  if (!source->SearchPage(inner, query, cursor, out, next, error))
    return false;
  for (browse::Entry& e : out)
    e.id = EncodeId(key, e.id);
  return true;
}

// The library first (what was played, pinned or left unfinished, by name:
// quick, and most likely what is meant), then every source that can
// search, most used first. A source that fails is left out, not the lot.
bool LibrarySource::SearchEverything(const std::string& query, browse::Listing& out,
                                     std::string& error) {
  const std::string needle = Lower(query);
  std::set<std::string> seen;
  auto add = [&](const Ref& ref) {
    if (Lower(ref.entry.name).find(needle) == std::string::npos)
      return;
    if (!seen.insert(ref.source_key + '\n' + ref.entry.id).second)
      return;
    browse::Entry e = ref.entry;
    e.id = EncodeId(ref.source_key, ref.entry.id);
    out.push_back(std::move(e));
  };
  for (const auto& item : library_.InProgress())
    add(item.first);
  for (const Recent& r : library_.RecentItems())
    add(r.ref);
  for (const Ref& ref : library_.Favorites())
    add(ref);

  // Most used first; those not remembered (none, normally) after them.
  std::vector<browse::SourcePtr> sources;
  for (const KnownSource& known : library_.KnownSources())
    if (const browse::SourcePtr s = registry_.Find(known.key))
      sources.push_back(s);
  for (const browse::SourcePtr& s : registry_.All())
    if (std::find(sources.begin(), sources.end(), s) == sources.end())
      sources.push_back(s);

  // All at once, each on a thread of its own: a search is mostly waiting
  // for a server, and the slowest should not add up with the others. They
  // are given up on together, as the search they belong to is.
  struct Job {
    browse::SourcePtr source;
    browse::Listing found;
    std::string why;
    bool ok = false;
  };
  std::vector<Job> jobs;
  for (const browse::SourcePtr& s : sources)
    if (s->CanSearch(s->RootId()))
      jobs.push_back({s, {}, {}, false});
  const net::CancelToken token = net::MakeCancelToken();
  std::atomic<size_t> left(jobs.size());
  std::vector<std::thread> threads;
  for (Job& job : jobs) {
    threads.emplace_back([&job, &left, token, query] {
      net::CancelScope scope(token);
      job.ok = job.source->Search(job.source->RootId(), query, job.found, job.why);
      left--;
    });
  }
  while (left > 0) {
    if (net::Cancelled())
      net::Cancel(token);
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  for (std::thread& t : threads)
    t.join();
  if (net::Cancelled()) {
    error = net::kCancelledError;
    return false;
  }

  std::string failures;
  for (Job& job : jobs) {
    const browse::SourcePtr& s = job.source;
    if (!job.ok) {
      if (!failures.empty())
        failures += "; ";
      failures += s->Name() + (job.why.empty() ? "" : ": " + job.why);
      continue;
    }
    for (browse::Entry& e : job.found) {
      if (!seen.insert(s->Key() + '\n' + e.id).second)
        continue;
      e.id = EncodeId(s->Key(), e.id);
      out.push_back(std::move(e));
    }
  }
  // Something found: what failed is not worth spoiling it for.
  if (out.empty() && !failures.empty()) {
    error = failures;
    return false;
  }
  return true;
}

bool LibrarySource::Resolve(const browse::Entry& entry, browse::Playable& out,
                            std::string& error) {
  std::string key, inner;
  if (!DecodeId(entry.id, key, inner))
    return browse::Source::Resolve(entry, out, error);
  if (const browse::SourcePtr source = registry_.Find(key)) {
    browse::Entry original = entry;
    original.id = inner;
    return source->Resolve(original, out, error);
  }
  // Offline source: the URI saved with the entry may well still work (a
  // file on a drive, a server whose discovery is merely late).
  if (!entry.uri.empty()) {
    out.uri = entry.uri;
    return true;
  }
  error = "that source is not available right now";
  return false;
}

} // namespace library
