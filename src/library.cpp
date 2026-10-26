// SPDX-License-Identifier: GPL-3.0-or-later
#include "library.h"

#include <algorithm>
#include <cstdio>
#include <ctime>

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
  progress_.clear();

  XMLDocument doc;
  if (doc.LoadFile(path.c_str()) != tinyxml2::XML_SUCCESS || !doc.RootElement())
    return; // first run, or unreadable: start empty

  for (const XMLElement* e = doc.RootElement()->FirstChildElement(); e;
       e = e->NextSiblingElement()) {
    Ref ref;
    if (!ReadRef(e, ref))
      continue;
    if (std::string(e->Name()) == "favorite") {
      favorites_.push_back(std::move(ref));
    } else if (std::string(e->Name()) == "progress") {
      Item item;
      item.progress.position = e->Int64Attribute("position", 0);
      item.progress.duration = e->Int64Attribute("total", 0);
      item.progress.watched = e->BoolAttribute("watched", false);
      item.progress.updated = e->Int64Attribute("updated", 0);
      item.ref = std::move(ref);
      const std::string id = Id(item.ref.source_key, item.ref.entry.id);
      progress_[id] = std::move(item);
    }
  }
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

  for (const Ref& ref : favorites_) {
    XMLElement* e = doc.NewElement("favorite");
    WriteRef(e, ref);
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
  for (const Ref& r : favorites_)
    if (r.source_key == key && r.entry.id == id)
      return true;
  return false;
}

bool Library::ToggleFavorite(const Ref& ref) {
  std::lock_guard<std::mutex> lock(mutex_);
  for (auto it = favorites_.begin(); it != favorites_.end(); ++it) {
    if (it->source_key == ref.source_key && it->entry.id == ref.entry.id) {
      favorites_.erase(it);
      Changed();
      return false;
    }
  }
  favorites_.push_back(ref);
  Changed();
  return true;
}

std::vector<Ref> Library::Favorites() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return favorites_;
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
    name_(kind == Kind::Favorites ? "Favorites" : "Continue watching") {
  UpdateDetail();
}

const char* LibrarySource::Icon() const {
  return kind_ == Kind::Favorites ? "⭐" : "⏯";
}

std::string LibrarySource::Key() const {
  return kind_ == Kind::Favorites ? "library:favorites" : "library:continue";
}

size_t LibrarySource::Count() const {
  return kind_ == Kind::Favorites ? library_.Favorites().size()
                                  : library_.InProgress().size();
}

void LibrarySource::UpdateDetail() {
  const size_t n = Count();
  if (n == 0)
    detail_ = kind_ == Kind::Favorites
      ? "Add folders and items from the options menu"
      : "Nothing started yet";
  else
    detail_ = std::to_string(n) + (n == 1 ? " item" : " items");
}

bool LibrarySource::Browse(const std::string& id, browse::Listing& out,
                           std::string& error) {
  if (id.empty()) {
    // The root: the stored refs, under ids that remember their source.
    // The progress and the source name are the app's to show (it looks
    // both up by the decoded id), so the entries go out as stored.
    auto add = [&out](const Ref& ref) {
      browse::Entry e = ref.entry;
      e.id = EncodeId(ref.source_key, ref.entry.id);
      out.push_back(std::move(e));
    };
    if (kind_ == Kind::Favorites) {
      for (const Ref& ref : library_.Favorites())
        add(ref);
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

bool LibrarySource::Resolve(const browse::Entry& entry, std::string& uri,
                            std::string& error) {
  std::string key, inner;
  if (!DecodeId(entry.id, key, inner))
    return browse::Source::Resolve(entry, uri, error);
  if (const browse::SourcePtr source = registry_.Find(key)) {
    browse::Entry original = entry;
    original.id = inner;
    return source->Resolve(original, uri, error);
  }
  // Offline source: the URI saved with the entry may well still work (a
  // file on a drive, a server whose discovery is merely late).
  if (!entry.uri.empty()) {
    uri = entry.uri;
    return true;
  }
  error = "that source is not available right now";
  return false;
}

} // namespace library
