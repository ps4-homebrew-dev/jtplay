// SPDX-License-Identifier: GPL-3.0-or-later
//
// browse::Source / browse::Provider backed by UPnP/DLNA media servers:
// SSDP discovery on one side, ContentDirectory Browse plus the DIDL ->
// browse::Entry conversion on the other.
#include "browse/dlna_source.h"

#include <algorithm>
#include <cstdio>

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

// Days since 1970-01-01 for a proleptic Gregorian date (Howard Hinnant's
// days_from_civil); avoids timegm(), which not every libc has.
int64_t DaysFromCivil(int64_t y, unsigned m, unsigned d) {
  y -= m <= 2;
  const int64_t era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = (unsigned)(y - era * 400);
  const unsigned doy = (153 * (m > 2 ? m - 3 : m + 9) + 2) / 5 + d - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + (int64_t)doe - 719468;
}

// dc:date is ISO 8601: "YYYY", "YYYY-MM-DD" or "YYYY-MM-DDThh:mm:ss[...]".
// Any zone suffix is ignored. Returns seconds since the epoch, 0 if none.
int64_t ParseDate(const std::string& s) {
  int y = 0, mo = 1, d = 1, h = 0, mi = 0, sec = 0;
  const int n = std::sscanf(s.c_str(), "%4d-%2d-%2dT%2d:%2d:%2d",
                            &y, &mo, &d, &h, &mi, &sec);
  if (n < 1 || y < 1900 || mo < 1 || mo > 12 || d < 1 || d > 31)
    return 0;
  return DaysFromCivil(y, (unsigned)mo, (unsigned)d) * 86400 +
         h * 3600 + mi * 60 + sec;
}

Entry Convert(const upnp::DidlObject& o) {
  Entry e;
  e.id = o.id;
  e.type = TypeOf(o);
  e.name = o.title;
  e.description = DescriptionOf(o);
  e.image = o.ArtUrl();  // absolute http URL, or ""
  e.uri = o.res_url;
  e.size = o.res_size;
  e.date = ParseDate(o.date);
  e.duration = o.res_duration;
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
