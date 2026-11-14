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

// Writes beside the target and renames over it.
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
  if (ok && std::rename(tmp.c_str(), path.c_str()) == 0)
    return true;
  std::remove(tmp.c_str());
  return false;
}

} // namespace util

#endif
