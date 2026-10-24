// SPDX-License-Identifier: GPL-3.0-or-later
//
// URL helpers plus the two requests UPnP needs: fetching a device
// description and posting a SOAP envelope to a control URL. The transport
// is net::HttpClient (libcurl), the same one the artwork cache and the
// plugins use.
//
// Media is never fetched through here; playback hands the URL straight to
// libavformat, which has its own HTTP stack.
#ifndef UPNP_HTTP_H
#define UPNP_HTTP_H

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace upnp {

// Broken-down absolute http:// URL.
struct Url {
  std::string host;
  uint16_t port = 80;
  std::string path = "/"; // includes the query string, if any

  // Parses "http://host[:port][/path]". Returns false for anything else
  // (https is not supported; UPnP control endpoints are plain http).
  static bool Parse(const std::string& url, Url& out);
};

// Resolves 'ref' (absolute URL, absolute path, or relative path) against
// 'base', per the subset of RFC 3986 that device descriptions actually use.
std::string ResolveUrl(const std::string& base, const std::string& ref);

struct HttpResponse {
  int status = 0;
  std::string body;
};

using Headers = std::vector<std::pair<std::string, std::string>>;

// One blocking request. Returns false on transport errors (DNS, connect,
// timeout) with a reason in 'error'; an HTTP error status is a successful
// request, reported in response.status. Blocking; worker thread only.
bool HttpRequest(const char* method, const std::string& url,
                 const Headers& headers, const std::string& body,
                 HttpResponse& response, std::string& error);

inline bool HttpGet(const std::string& url, HttpResponse& r, std::string& e) {
  return HttpRequest("GET", url, {}, {}, r, e);
}

} // namespace upnp

#endif
