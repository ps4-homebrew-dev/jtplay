// SPDX-License-Identifier: GPL-3.0-or-later
//
// browse::Source / browse::Provider backed by UPnP/DLNA media servers.
#ifndef BROWSE_DLNA_SOURCE_H
#define BROWSE_DLNA_SOURCE_H

#include "browse/source.h"
#include "upnp/dlna.h"

namespace browse {

class DlnaSource : public Source {
public:
  explicit DlnaSource(upnp::MediaServer server);

  const std::string& Name() const override { return server_.friendly_name; }
  const std::string& Detail() const override { return detail_; }
  const char* Icon() const override { return "🖥"; }

  std::string RootId() const override { return "0"; } // ContentDirectory root

  bool Browse(const std::string& id, Listing& out, std::string& error) override;

private:
  upnp::MediaServer server_;
  std::string detail_; // "<model>  -  <host>", best effort
};

// SSDP discovery; every reachable MediaServer becomes one DlnaSource.
class DlnaProvider : public Provider {
public:
  explicit DlnaProvider(int discovery_wait_ms);

  const char* Name() const override { return "DLNA"; }
  bool Discover(std::vector<SourcePtr>& out, std::string& error) override;

private:
  int discovery_wait_ms_;
};

} // namespace browse

#endif
