// SPDX-License-Identifier: GPL-3.0-or-later
#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

#include "artcache.h"
#include "net/http_client.h"

namespace artcache {
namespace {

// Artwork is decoration; nothing here is worth a long stall on the worker
// thread or a large allocation on a console with no swap.
constexpr size_t kMaxImageBytes = 12u << 20;
constexpr long kTimeoutMs = 15000;

// Files kept across runs. A few hundred covers is a couple of tens of MiB,
// and the oldest ones are dropped on the next start.
constexpr size_t kMaxCachedFiles = 512;

std::mutex g_mutex;               // guards everything below
std::string g_dir;                // "" = cache unavailable
std::map<std::string, std::string> g_index;  // uri -> path ("" = failed)
std::map<std::string, std::string> g_ondisk; // "<hash>" -> path, from Initialize

// Downloads are serialized: one shared connection is plenty for artwork and
// it keeps TLS sessions warm across a listing.
std::mutex g_fetch_mutex;

// Tiny FNV-1a, good enough to key cache filenames by URI.
uint64_t HashUri(const std::string& s) {
  uint64_t h = 1469598103934665603ull;
  for (unsigned char c : s) {
    h ^= c;
    h *= 1099511628211ull;
  }
  return h;
}

std::string HashKey(const std::string& uri) {
  char buf[17];
  std::snprintf(buf, sizeof(buf), "%016llx", (unsigned long long)HashUri(uri));
  return std::string(buf);
}

// SDL_image picks the decoder from the extension we store under, so sniff
// the actual bytes rather than trusting the server's Content-Type. A server
// that answers an <img> request with an HTML error page is the common case
// this catches.
const char* SniffImageExt(const std::string& b) {
  if (b.size() >= 3 && (unsigned char)b[0] == 0xff && (unsigned char)b[1] == 0xd8)
    return "jpg";
  if (b.size() >= 8 && b.compare(0, 4, "\x89PNG") == 0)
    return "png";
  if (b.size() >= 6 && (b.compare(0, 6, "GIF87a") == 0 || b.compare(0, 6, "GIF89a") == 0))
    return "gif";
  if (b.size() >= 2 && b[0] == 'B' && b[1] == 'M')
    return "bmp";
  if (b.size() >= 12 && b.compare(0, 4, "RIFF") == 0 && b.compare(8, 4, "WEBP") == 0)
    return "webp";
  return nullptr;
}

// True for a URI RmlUi can already load: "file:///path" or a bare "/path".
// 'out' gets the plain path.
bool LocalPath(const std::string& uri, std::string& out) {
  if (uri.compare(0, 7, "file://") == 0) {
    out = uri.substr(7);
    return true;
  }
  if (!uri.empty() && uri[0] == '/') {
    out = uri;
    return true;
  }
  return false;
}

// Splits "art-<16 hex>.<ext>" back into its hash. Returns "" for anything
// else in the directory, which is then left alone.
std::string HashFromFilename(const std::string& name) {
  if (name.compare(0, 4, "art-") != 0 || name.size() < 4 + 16 + 2)
    return {};
  if (name[4 + 16] != '.')
    return {};
  const std::string hash = name.substr(4, 16);
  if (hash.find_first_not_of("0123456789abcdef") != std::string::npos)
    return {};
  return hash;
}

// Keeps the newest kMaxCachedFiles entries and unlinks the rest. Called once,
// from Initialize(), with the index already built.
void TrimLocked() {
  if (g_ondisk.size() <= kMaxCachedFiles)
    return;

  std::vector<std::pair<time_t, std::string>> by_age; // (mtime, hash)
  by_age.reserve(g_ondisk.size());
  for (const auto& entry : g_ondisk) {
    struct stat st = {};
    const time_t mtime = (::stat(entry.second.c_str(), &st) == 0) ? st.st_mtime : 0;
    by_age.emplace_back(mtime, entry.first);
  }
  std::sort(by_age.begin(), by_age.end()); // oldest first

  const size_t drop = by_age.size() - kMaxCachedFiles;
  for (size_t i = 0; i < drop; i++) {
    auto it = g_ondisk.find(by_age[i].second);
    if (it == g_ondisk.end())
      continue;
    ::remove(it->second.c_str());
    g_ondisk.erase(it);
  }
}

} // namespace

bool Initialize(const std::string& dir) {
  std::lock_guard<std::mutex> lock(g_mutex);
  g_dir.clear();
  g_index.clear();
  g_ondisk.clear();

  if (dir.empty())
    return false;
  if (::mkdir(dir.c_str(), 0755) != 0 && errno != EEXIST)
    return false;

  // Whatever a previous run downloaded is still good: index it by hash so
  // Lookup() resolves without touching the network.
  if (DIR* d = ::opendir(dir.c_str())) {
    while (struct dirent* de = ::readdir(d)) {
      const std::string name = de->d_name;
      const std::string hash = HashFromFilename(name);
      if (!hash.empty())
        g_ondisk[hash] = dir + "/" + name;
    }
    ::closedir(d);
  }

  g_dir = dir;
  TrimLocked();
  return true;
}

void Shutdown() {
  std::lock_guard<std::mutex> lock(g_mutex);
  g_dir.clear();
  g_index.clear();
  g_ondisk.clear();
}

Status Lookup(const std::string& uri, std::string& path) {
  path.clear();
  if (uri.empty())
    return Status::Failed;

  // Local artwork never enters the cache; it is already a file.
  if (LocalPath(uri, path))
    return Status::Ready;

  if (uri.compare(0, 7, "http://") != 0 && uri.compare(0, 8, "https://") != 0)
    return Status::Failed; // nothing else can be downloaded

  std::lock_guard<std::mutex> lock(g_mutex);
  if (g_dir.empty())
    return Status::Failed;

  auto it = g_index.find(uri);
  if (it != g_index.end()) {
    path = it->second;
    return path.empty() ? Status::Failed : Status::Ready;
  }

  auto disk = g_ondisk.find(HashKey(uri));
  if (disk != g_ondisk.end()) {
    path = disk->second;
    g_index[uri] = path;
    return Status::Ready;
  }

  return Status::Missing;
}

std::string Fetch(const std::string& uri) {
  std::string path;
  const Status status = Lookup(uri, path);
  if (status != Status::Missing)
    return path; // Ready gives a path, Failed gives ""

  std::string dir;
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    dir = g_dir;
  }
  if (dir.empty())
    return {};

  std::lock_guard<std::mutex> fetching(g_fetch_mutex);

  // One client for the life of the process; the mutex above is what makes
  // that safe, since HttpClient itself is not.
  static net::HttpClient client;

  net::HttpClient::Request req;
  req.url = uri;
  req.timeout_ms = kTimeoutMs;
  req.max_bytes = kMaxImageBytes;

  net::HttpClient::Response res;
  std::string error;

  std::string full; // stays empty on any failure
  if (client.Perform(req, res, error) && res.status == 200 && !res.body.empty()) {
    if (const char* ext = SniffImageExt(res.body)) {
      const std::string name = "art-" + HashKey(uri) + "." + ext;
      const std::string final_path = dir + "/" + name;

      // Write beside the target and rename: Lookup() runs on the UI thread
      // and must never see a half-written file.
      const std::string tmp_path = final_path + ".tmp";
      bool ok = false;
      if (FILE* f = std::fopen(tmp_path.c_str(), "wb")) {
        ok = std::fwrite(res.body.data(), 1, res.body.size(), f) == res.body.size();
        ok = (std::fclose(f) == 0) && ok;
      }
      if (ok && ::rename(tmp_path.c_str(), final_path.c_str()) == 0)
        full = final_path;
      else
        ::remove(tmp_path.c_str());
    }
  }

  std::lock_guard<std::mutex> lock(g_mutex);
  g_index[uri] = full; // "" is remembered too: do not retry a dead URL
  if (!full.empty())
    g_ondisk[HashKey(uri)] = full;
  return full;
}

} // namespace artcache
