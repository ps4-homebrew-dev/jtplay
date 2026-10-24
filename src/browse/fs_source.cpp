// SPDX-License-Identifier: GPL-3.0-or-later

#include <dirent.h>
#include <sys/stat.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstring>

#include "browse/fs_source.h"

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
    // Deliberately permissive on the video side; ffmpeg will tell us soon
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

    bool CaseInsensitiveLess(const std::string& a, const std::string& b) {
      const size_t n = std::min(a.size(), b.size());
      for (size_t i = 0; i < n; i++) {
	const int ca = std::tolower((unsigned char)a[i]);
	const int cb = std::tolower((unsigned char)b[i]);
	if (ca != cb)
	  return ca < cb;
      }
      return a.size() < b.size();
    }

    std::string TrimTrailingSlashes(std::string path) {
      while (path.size() > 1 && path.back() == '/')
	path.pop_back();
      return path;
    }

    // Ids stay plain absolute paths (that is what Browse() takes); the
    // playable and artwork URIs get a scheme, like every other source.
    // Not percent-encoded: both consumers (ffmpeg's file protocol and the
    // app's artwork cache) strip the prefix and open the rest verbatim,
    // which is what a path with spaces or '#' in it needs.
    std::string FileUri(const std::string& path) {
      return "file://" + path;
    }

  }

  FsSource::FsSource(std::string name, std::string root)
    : name_(std::move(name)), root_(TrimTrailingSlashes(std::move(root))) {}

  bool FsSource::Browse(const std::string& id, Listing& out, std::string& error) {
    const std::string dir = TrimTrailingSlashes(id);

    // Ids come back from the UI verbatim, but stay paranoid: only paths
    // inside this source's root are browsable.
    if (dir.compare(0, root_.size(), root_) != 0 ||
	(dir.size() > root_.size() && dir[root_.size()] != '/')) {
      error = "path outside this source";
      return false;
    }

    DIR* d = ::opendir(dir.c_str());
    if (!d) {
      error = "cannot open " + dir + ": " + std::strerror(errno);
      return false;
    }

    std::string cover; // cover art for this directory, if any
    std::vector<Entry> folders;
    std::vector<Entry> files;

    while (struct dirent* de = ::readdir(d)) {
      const std::string name = de->d_name;
      if (name.empty() || name[0] == '.')
	continue;

      const std::string path = dir + "/" + name;
      struct stat st = {};
      if (::stat(path.c_str(), &st) != 0)
	continue;

      if (S_ISDIR(st.st_mode)) {
	Entry e;
	e.id = path;
	e.type = Entry::Type::Folder;
	e.name = name;
	folders.push_back(std::move(e));
	continue;
      }
      if (!S_ISREG(st.st_mode))
	continue;

      Entry::Type type = Entry::Type::Other;
      if (!ClassifyExt(LowerExt(name), type))
	continue;
      if (cover.empty() && IsCoverName(name))
	cover = path;

      Entry e;
      e.id = path;
      e.type = type;
      e.name = name;
      e.uri = FileUri(path);
      if (type == Entry::Type::Image)
	e.image = e.uri; // the image previews itself in the details pane
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
      return CaseInsensitiveLess(a.name, b.name);
    };
    std::sort(folders.begin(), folders.end(), by_name);
    std::sort(files.begin(), files.end(), by_name);

    out = std::move(folders);
    out.insert(out.end(),
	       std::make_move_iterator(files.begin()),
	       std::make_move_iterator(files.end()));
    return true;
  }
}
