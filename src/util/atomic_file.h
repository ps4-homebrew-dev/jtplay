// SPDX-License-Identifier: GPL-3.0-or-later
//
// Small files, whole: read one into a string, or replace one so that a
// crash or a power cut part-way through leaves the old file or the new one,
// never half of either. (A console is switched off at the wall more often
// than it is shut down.)
#ifndef UTIL_ATOMIC_FILE_H
#define UTIL_ATOMIC_FILE_H

#include <cstdio>
#include <string>

#include <fcntl.h>
#include <unistd.h>

namespace util {

inline bool ReadFile(const std::string& path, std::string& out, size_t max_bytes = 64u << 20) {
  FILE* f = std::fopen(path.c_str(), "rb");
  if (!f)
    return false;
  out.clear();
  char buf[16384];
  size_t n;
  bool ok = true;
  while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) {
    if (out.size() + n > max_bytes) {
      ok = false;
      break;
    }
    out.append(buf, n);
  }
  ok = ok && !std::ferror(f);
  std::fclose(f);
  return ok;
}

// Writes beside the target and renames over it. Every write is checked: on
// a full disk this fails and the old file stays as it was.
inline bool WriteFileAtomic(const std::string& path, const std::string& contents) {
  const std::string tmp = path + ".tmp";
  FILE* f = std::fopen(tmp.c_str(), "wb");
  if (!f)
    return false;
  bool ok = std::fwrite(contents.data(), 1, contents.size(), f) == contents.size();
  ok = std::fflush(f) == 0 && ok;
  // On disk before it takes the old file's place; without this a rename
  // can outlive the data it names.
  ok = ::fsync(::fileno(f)) == 0 && ok;
  ok = std::fclose(f) == 0 && ok;
  if (!ok || std::rename(tmp.c_str(), path.c_str()) != 0) {
    std::remove(tmp.c_str());
    return false;
  }
  // And the rename itself (where a directory can be synced at all).
  const size_t slash = path.rfind('/');
  const std::string dir = slash == std::string::npos ? "." : slash == 0 ? "/" : path.substr(0, slash);
  const int fd = ::open(dir.c_str(), O_RDONLY);
  if (fd >= 0) {
    ::fsync(fd);
    ::close(fd);
  }
  return true;
}

// For a file that is read once at start and replaced now and then (the
// library, the settings): reads 'path', or, when that is missing, empty or
// not 'valid', the copy kept beside it as path + ".bak" (a broken file is
// moved aside to path + ".broken", where it is not written over). A good
// 'path' becomes the new backup. Returns false when there is neither, as
// on a first start; 'restored' says the backup was used, so the caller
// writes the file again.
template <typename Valid>
bool ReadWithBackup(const std::string& path, std::string& out, Valid valid, bool& restored) {
  const std::string backup = path + ".bak";
  restored = false;
  std::string old;
  if (ReadFile(path, out) && !out.empty() && valid(out)) {
    if (!(ReadFile(backup, old) && old == out))
      WriteFileAtomic(backup, out);
    return true;
  }
  if (::access(path.c_str(), F_OK) == 0) {
    std::fprintf(stderr, "%s is damaged; kept as %s.broken\n", path.c_str(), path.c_str());
    std::rename(path.c_str(), (path + ".broken").c_str());
  }
  if (ReadFile(backup, out) && !out.empty() && valid(out)) {
    std::fprintf(stderr, "%s: restored from %s\n", path.c_str(), backup.c_str());
    restored = true;
    return true;
  }
  out.clear();
  return false;
}

} // namespace util

#endif
