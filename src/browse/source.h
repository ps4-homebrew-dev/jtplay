// SPDX-License-Identifier: GPL-3.0-or-later
//
// The browse abstraction: a Source is one browsable tree of media (a DLNA
// server, a local directory, ...) and a Provider discovers Sources of one
// kind. The app only ever talks to these interfaces; everything protocol
// specific lives in the implementations (dlna_source.cpp, fs_source.cpp).
//
// All Browse()/Discover() calls are blocking I/O; the app invokes them from
// its worker thread only. Sources are handed around as shared_ptr so an
// in-flight browse survives a rescan that replaces the source list.
#ifndef BROWSE_SOURCE_H
#define BROWSE_SOURCE_H

#include <memory>
#include <string>
#include <vector>

namespace browse {

// One entry of a listing: either a folder or a (hopefully) playable item.
//
// Six fields, deliberately: what the UI draws and what the player opens,
// and nothing else. It is not modelled on any one protocol; every source
// projects its native objects onto this, dropping whatever does not fit.
struct Entry {
  enum class Type { Folder, Audio, Video, Image, Other };

  // Opaque and source specific: a path, a DIDL object id, a uuid, ...
  // Handed back to Browse() for folders and to Resolve() for items, so it
  // has to mean something to the source that minted it and to nobody else.
  std::string id;
  Type type = Type::Other;

  // What to show: one line of title, one line of anything else worth
  // knowing ("Miles Davis - Kind of Blue", a plot summary, a station
  // tagline). Both are display strings; nothing parses them.
  std::string name;
  std::string description;

  // Artwork and media, as URIs the app can open: file://, http://,
  // https://, or anything else ffmpeg understands. 'image' is empty when
  // there is no artwork; 'uri' may be empty when the source mints one on
  // demand (see Source::Resolve).
  std::string image;
  std::string uri;

  bool IsFolder() const { return type == Type::Folder; }
  bool IsAudio() const { return type == Type::Audio; }
  bool IsVideo() const { return type == Type::Video; }
  bool IsImage() const { return type == Type::Image; }
};

using Listing = std::vector<Entry>;

// One browsable tree of media.
class Source {
public:
  virtual ~Source() = default;

  virtual const std::string& Name() const = 0;   // row title
  virtual const std::string& Detail() const = 0; // row subtitle (host, path)
  virtual const char* Icon() const = 0;          // emoji glyph for the row

  // Id of the root folder; passed back to Browse() to start.
  virtual std::string RootId() const = 0;

  // Lists the direct children of 'id'. Blocking; worker thread only.
  virtual bool Browse(const std::string& id, Listing& out,
                      std::string& error) = 0;

  // Turns 'entry' into something ffmpeg can open, immediately before
  // playback starts. The default hands back the uri captured while
  // browsing, which is all a DLNA server or a local file needs; sources
  // whose URIs carry a token that expires between listing and playing
  // override this and mint a fresh one from entry.id.
  //
  // Blocking (it may do I/O); worker thread only.
  virtual bool Resolve(const Entry& entry, std::string& uri,
                       std::string& error) {
    uri = entry.uri;
    if (uri.empty()) {
      error = "this item has nothing to play";
      return false;
    }
    return true;
  }
};

using SourcePtr = std::shared_ptr<Source>;

// Discovers Sources of one kind (SSDP scan, mounted filesystems, ...).
class Provider {
public:
  virtual ~Provider() = default;

  virtual const char* Name() const = 0; // for error messages

  // Appends whatever it finds to 'out'. Returning false (with a reason in
  // 'error') means the scan itself failed; an empty result is not an error.
  // Blocking; worker thread only.
  virtual bool Discover(std::vector<SourcePtr>& out, std::string& error) = 0;
};

} // namespace browse

#endif
