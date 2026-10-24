// SPDX-License-Identifier: GPL-3.0-or-later
//
// Artwork cache: the one place where an Entry::image URI becomes something
// RmlUi can draw.
//
// RmlUi loads textures through its file interface, so it can only show a
// local path. Local artwork (file:// or a bare path) is handed straight
// back; remote artwork (http:// or https://) is downloaded once into a
// cache directory and every later request resolves to that file. No source
// implementation does any of this itself: dlna_source, fs_source and the
// JavaScript plugins all just put a URI in Entry::image and let the cache
// sort out the rest.
//
// Fetch() blocks on network I/O and is for worker threads only. Lookup()
// never touches the network and is safe to call from the UI thread; the two
// share one index behind a mutex.
#ifndef ARTCACHE_H
#define ARTCACHE_H

#include <string>

namespace artcache {

// State of one artwork URI, as far as the cache knows.
enum class Status {
  Ready,   // 'path' is a local file the renderer can load
  Missing, // not cached yet; someone has to call Fetch() on a worker thread
  Failed,  // no artwork to be had, and not worth asking again
};

// Creates 'dir' (one level; the parent must exist) and indexes whatever a
// previous run left there, so a restart does not re-download every cover.
// Returns false if the directory is unusable, in which case remote artwork
// is quietly skipped and local artwork still works.
bool Initialize(const std::string& dir);

// Drops the in-memory index. The files stay: they are still valid next
// time, and Initialize() trims the directory back to size on the way in.
void Shutdown();

// Resolves 'uri' without any I/O. Fills 'path' and returns Ready for local
// artwork and for anything already in the cache.
Status Lookup(const std::string& uri, std::string& path);

// Lookup(), and on Missing the download that makes it Ready: fetches the
// URI, checks that the bytes really are an image, stores them under a name
// derived from the URI, and returns the path ("" on any failure, which is
// remembered so a dead URL is not retried for the rest of the run).
//
// Blocking; worker thread only.
std::string Fetch(const std::string& uri);

} // namespace artcache

#endif
