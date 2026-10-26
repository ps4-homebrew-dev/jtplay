// SPDX-License-Identifier: GPL-3.0-or-later
#include "browse/fs_source.h"

#include <dirent.h>
#include <sys/stat.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <utility>

#include "browse/natural_sort.h"

namespace browse {

namespace {

std::string LowerExt(const std::string& name) {
  const size_t dot = name.rfind('.');
  if (dot == std::string::npos || dot + 1 >= name.size())
    return {};
  std::string ext = name.substr(dot + 1);
  for (char& c : ext)
    c = (char)std::tolower((unsigned char)c);
  return ext;
}

// Extension -> entry type; anything not listed here is not shown at all.
// Deliberately permissive on the video side; the player will tell us soon
// enough if it cannot open something.
bool ClassifyExt(const std::string& ext, Entry::Type& type) {
  static const std::pair<const char*, Entry::Type> kMap[] = {
    // Audio
    {"mp3",  Entry::Type::Audio},
    {"flac", Entry::Type::Audio},
    {"ogg",  Entry::Type::Audio},
    {"oga",  Entry::Type::Audio},
    {"opus", Entry::Type::Audio},
    {"m4a",  Entry::Type::Audio},
    {"aac",  Entry::Type::Audio},
    {"wav",  Entry::Type::Audio},
    {"wma",  Entry::Type::Audio},
    {"ape",  Entry::Type::Audio},
    {"mka",  Entry::Type::Audio},
    // Video
    {"mkv",  Entry::Type::Video},
    {"mp4",  Entry::Type::Video},
    {"m4v",  Entry::Type::Video},
    {"mov",  Entry::Type::Video},
    {"avi",  Entry::Type::Video},
    {"webm", Entry::Type::Video},
    {"ts",   Entry::Type::Video},
    {"m2ts", Entry::Type::Video},
    {"mts",  Entry::Type::Video},
    {"mpg",  Entry::Type::Video},
    {"mpeg", Entry::Type::Video},
    {"vob",  Entry::Type::Video},
    {"wmv",  Entry::Type::Video},
    {"flv",  Entry::Type::Video},
    {"3gp",  Entry::Type::Video},
    // Images
    {"jpg",  Entry::Type::Image},
    {"jpeg", Entry::Type::Image},
    {"png",  Entry::Type::Image},
    {"gif",  Entry::Type::Image},
    {"bmp",  Entry::Type::Image},
    {"webp", Entry::Type::Image},
  };
  for (const auto& m : kMap) {
    if (ext == m.first) {
      type = m.second;
      return true;
    }
  }
  return false;
}

} // namespace

bool ClassifyFileName(const std::string& name, Entry::Type& type) {
  return ClassifyExt(LowerExt(name), type);
}

namespace {

// Case-insensitive match against the usual cover art file names.
bool IsCoverName(const std::string& name) {
  std::string lower = name;
  for (char& c : lower)
    c = (char)std::tolower((unsigned char)c);
  for (const char* c : {"cover.jpg", "cover.jpeg", "cover.png", "folder.jpg",
                        "folder.jpeg", "folder.png", "front.jpg", "front.png",
                        "albumart.jpg", "albumart.png"}) {
    if (lower == c)
      return true;
  }
  return false;
}

std::string TrimTrailingSlashes(std::string path) {
  while (path.size() > 1 && path.back() == '/')
    path.pop_back();
  return path;
}

// Ids stay plain absolute paths (that is what Browse() takes); the playable
// and artwork URIs get a scheme, like every other source. Not
// percent-encoded: both consumers (the player's file protocol and the
// app's artwork cache) strip the prefix and open the rest verbatim, which
// is what a path with spaces or '#' in it needs.
std::string FileUri(const std::string& path) {
  return "file://" + path;
}

} // namespace

FsSource::FsSource(std::string name, std::string root)
  : name_(std::move(name)), root_(TrimTrailingSlashes(std::move(root))) {}

bool FsSource::Browse(const std::string& id, Listing& out, std::string& error) {
  const std::string dir = TrimTrailingSlashes(id);

  // Ids come back from the UI verbatim, but stay paranoid: only paths
  // inside this source's root are browsable.
  if (root_ != "/" &&
      (dir.compare(0, root_.size(), root_) != 0 ||
       (dir.size() > root_.size() && dir[root_.size()] != '/'))) {
    error = "path outside this source";
    return false;
  }

  DIR* d = ::opendir(dir.c_str());
  if (!d) {
    error = "cannot open " + dir + ": " + std::strerror(errno);
    return false;
  }

  const std::string prefix = dir == "/" ? std::string() : dir;
  std::string cover; // cover art for this directory, if any
  Listing folders;
  Listing files;

  while (struct dirent* de = ::readdir(d)) {
    const std::string name = de->d_name;
    if (name.empty() || name[0] == '.')
      continue;

    const std::string path = prefix + "/" + name;
    struct stat st = {};
    if (::stat(path.c_str(), &st) != 0)
      continue;

    Entry e;
    e.id = path;
    e.name = name;
    e.date = (int64_t)st.st_mtime;

    if (S_ISDIR(st.st_mode)) {
      e.type = Entry::Type::Folder;
      folders.push_back(std::move(e));
      continue;
    }
    if (!S_ISREG(st.st_mode))
      continue;
    if (!ClassifyFileName(name, e.type))
      continue;
    if (cover.empty() && IsCoverName(name))
      cover = path;

    e.size = (int64_t)st.st_size;
    e.uri = FileUri(path);
    if (e.IsImage())
      e.image = e.uri; // the image previews itself in the info panel
    files.push_back(std::move(e));
  }
  ::closedir(d);

  // Audio tracks inherit the directory's cover art, album-folder style.
  if (!cover.empty()) {
    for (Entry& e : files) {
      if (e.IsAudio() && e.image.empty())
        e.image = FileUri(cover);
    }
  }

  auto by_name = [](const Entry& a, const Entry& b) {
    return NaturalLess(a.name, b.name);
  };
  std::sort(folders.begin(), folders.end(), by_name);
  std::sort(files.begin(), files.end(), by_name);

  out = std::move(folders);
  out.insert(out.end(),
             std::make_move_iterator(files.begin()),
             std::make_move_iterator(files.end()));
  return true;
}

namespace {

bool IsDirectory(const std::string& path) {
  struct stat st = {};
  return ::stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

#if defined(__ORBIS__)
// A mount point with nothing in it is a USB port without a drive.
bool HasEntries(const std::string& path) {
  DIR* d = ::opendir(path.c_str());
  if (!d)
    return false;
  bool any = false;
  while (struct dirent* de = ::readdir(d)) {
    if (std::strcmp(de->d_name, ".") != 0 && std::strcmp(de->d_name, "..") != 0) {
      any = true;
      break;
    }
  }
  ::closedir(d);
  return any;
}
#endif

} // namespace

bool FsProvider::Discover(std::vector<SourcePtr>& out, std::string&) {
#if defined(__ORBIS__)
  for (int i = 0; i < 8; i++) {
    const std::string path = "/mnt/usb" + std::to_string(i);
    if (IsDirectory(path) && HasEntries(path))
      out.push_back(std::make_shared<FsSource>("USB drive " + std::to_string(i + 1), path));
  }
  if (IsDirectory("/mnt/ext0") && HasEntries("/mnt/ext0"))
    out.push_back(std::make_shared<FsSource>("Extended storage", "/mnt/ext0"));
  if (IsDirectory("/data"))
    out.push_back(std::make_shared<FsSource>("Internal storage", "/data"));
#else
  if (const char* home = std::getenv("HOME")) {
    if (*home && IsDirectory(home) && std::string(home) != "/")
      out.push_back(std::make_shared<FsSource>("Home folder", home));
  }
#endif
  out.push_back(std::make_shared<FsSource>("Entire filesystem", "/"));
  return true;
}

} // namespace browse
