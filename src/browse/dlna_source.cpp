// SPDX-License-Identifier: GPL-3.0-or-later
//
// browse::Source / browse::Provider backed by UPnP/DLNA media servers:
// SSDP discovery on one side, ContentDirectory Browse plus the DIDL ->
// browse::Entry conversion on the other.
#include "browse/dlna_source.h"

#include <algorithm>

#include "upnp/http.h"
#include "upnp/ssdp.h"

namespace browse {

namespace {

std::string HostOf(const std::string& url) {
  upnp::Url u;
  if (!upnp::Url::Parse(url, u))
    return url;
  return u.host;
}

Entry::Type TypeOf(const upnp::DidlObject& o) {
  if (o.container)
    return Entry::Type::Folder;
  if (o.IsAudio())
    return Entry::Type::Audio;
  if (o.IsVideo())
    return Entry::Type::Video;
  if (o.IsImage())
    return Entry::Type::Image;
  return Entry::Type::Other;
}

// DIDL scatters its metadata over half a dozen elements; the entry has one
// description line, so pick the ones a viewer actually reads.
std::string DescriptionOf(const upnp::DidlObject& o) {
  std::string desc;
  auto add = [&desc](const std::string& value) {
    if (!value.empty())
      desc += (desc.empty() ? "" : "  -  ") + value;
  };
  add(o.artist);
  add(o.album);
  if (desc.empty()) {
    add(o.genre);
    add(o.date);
  }
  return desc;
}

Entry Convert(const upnp::DidlObject& o) {
  Entry e;
  e.id = o.id;
  e.type = TypeOf(o);
  e.name = o.title;
  e.description = DescriptionOf(o);
  e.image = o.ArtUrl();  // absolute http URL, or ""
  e.uri = o.res_url;
  return e;
}

} // namespace

// ---------------------------------------------------------------------------
// DlnaSource
// ---------------------------------------------------------------------------

DlnaSource::DlnaSource(upnp::MediaServer server) : server_(std::move(server)) {
  const std::string host = HostOf(server_.location);
  detail_ = server_.model.empty() ? host : server_.model + "  -  " + host;
}

bool DlnaSource::Browse(const std::string& id, Listing& out,
                        std::string& error) {
  upnp::BrowseResult result;
  if (!upnp::Browse(server_, id, result, error))
    return false;
  out.reserve(result.objects.size());
  for (const upnp::DidlObject& o : result.objects)
    out.push_back(Convert(o));
  return true;
}

// ---------------------------------------------------------------------------
// DlnaProvider
// ---------------------------------------------------------------------------

DlnaProvider::DlnaProvider(int discovery_wait_ms)
  : discovery_wait_ms_(discovery_wait_ms) {}

bool DlnaProvider::Discover(std::vector<SourcePtr>& out, std::string& error) {
  std::vector<upnp::SsdpResult> found =
    upnp::SsdpSearch(discovery_wait_ms_, error);
  if (found.empty() && !error.empty())
    return false;

  std::vector<upnp::MediaServer> servers;
  for (const upnp::SsdpResult& r : found) {
    upnp::MediaServer server;
    std::string derr;
    if (upnp::DescribeServer(r.location, server, derr))
      servers.push_back(std::move(server));
  }

  // The same server can answer on several interfaces with different
  // LOCATIONs but one UDN.
  std::sort(servers.begin(), servers.end(),
    [](const upnp::MediaServer& a, const upnp::MediaServer& b) {
      return a.udn < b.udn;
    });
  servers.erase(std::unique(servers.begin(), servers.end(),
    [](const upnp::MediaServer& a, const upnp::MediaServer& b) {
      return !a.udn.empty() && a.udn == b.udn;
    }), servers.end());
  std::sort(servers.begin(), servers.end(),
    [](const upnp::MediaServer& a, const upnp::MediaServer& b) {
      return a.friendly_name < b.friendly_name;
    });

  for (upnp::MediaServer& s : servers)
    out.push_back(std::make_shared<DlnaSource>(std::move(s)));
  return true;
}

} // namespace browse
