// SPDX-License-Identifier: GPL-3.0-or-later
//
// browse::Source over a local directory tree, and the provider that offers
// the filesystem root.
#ifndef BROWSE_FS_SOURCE_H
#define BROWSE_FS_SOURCE_H

#include "browse/source.h"

namespace browse {

class FsSource : public Source {
public:
  FsSource(std::string name, std::string root);

  const std::string& Name() const override { return name_; }
  const std::string& Detail() const override { return root_; }
  const char* Icon() const override { return "💾"; }
  std::string Key() const override { return "fs:" + root_; }
  std::string RootId() const override { return root_; }
  bool Browse(const std::string& id, Listing& out, std::string& error) override;

private:
  std::string name_;
  std::string root_;
};

// Offers the places media usually lives as their own sources, so they are
// one press away: USB drives and /data on the console, the home directory
// on a PC; plus the whole filesystem as a last resort.
class FsProvider : public Provider {
public:
  const char* Name() const override { return "fs"; }
  bool Discover(std::vector<SourcePtr>& out, std::string& error) override;
};

} // namespace browse

#endif
