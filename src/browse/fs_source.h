// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "browse/source.h"

namespace browse {
  class FsSource : public Source {
  public:
    FsSource(std::string name, std::string root);

    const std::string& Name() const override { return name_; }
    const std::string& Detail() const override { return root_; }
    const char* Icon() const override { return "💾"; }
    std::string RootId() const override { return root_; }
    bool Browse(const std::string& id, Listing& out, std::string& error) override;

  private:
    std::string name_;
    std::string root_;
  };

  class FsProvider : public Provider {
  public:
    const char* Name() const override { return "fs"; }

    bool Discover(std::vector<SourcePtr>& out, std::string& error) override {
      out.push_back(std::make_shared<FsSource>("Local filesystem", "/"));
      return true;
    }
  }; 
}
