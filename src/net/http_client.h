// SPDX-License-Identifier: GPL-3.0-or-later
//
// One blocking HTTP client on top of libcurl: https, redirects, content
// encodings and CA-bundle discovery, in one place. The plugin 'http' global
// and the artwork cache both use it, so anything a plugin can reach is also
// something the UI can display.
//
// (upnp/http.h is a thin layer over this one for SOAP control endpoints on
// the LAN: the UPnP user agent, tighter limits.)
//
// A request gives up early when the work it is part of is cancelled (see
// net/cancel.h); Perform() then fails with net::kCancelledError.
//
// Not thread safe: one instance belongs to one thread, or to whoever holds
// the mutex that guards it. Requests block, so callers are worker threads.
#ifndef NET_HTTP_CLIENT_H
#define NET_HTTP_CLIENT_H

#include <cstddef>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include <curl/curl.h>

namespace net {

class HttpClient {
public:
  struct Request {
    std::string method = "GET";
    std::string url;
    std::vector<std::pair<std::string, std::string>> headers;
    std::string body;
    bool has_body = false;
    bool follow_redirects = true;

    // Gives up when no data has moved for this long. This is not a cap on
    // the whole transfer: a large listing or image on a slow link may take
    // as long as it needs, as long as bytes keep arriving.
    long stall_timeout_ms = 30000;
    // Optional hard limit on the whole request, 0 = none.
    long deadline_ms = 0;

    // The body is held outside any managed heap, so every caller states how
    // much it is willing to hold. Exceeding it aborts the transfer, as an
    // error unless 'truncate' is set: then the first max_bytes are the body
    // and Response::truncated says so.
    size_t max_bytes = 8u << 20;
    bool truncate = false;
  };

  struct Response {
    long status = 0;
    std::string url;                            // after redirects
    std::map<std::string, std::string> headers; // keys lower-cased
    std::string body;
    bool truncated = false;                     // cut at Request::max_bytes
  };

  HttpClient();
  ~HttpClient();

  HttpClient(const HttpClient&) = delete;
  HttpClient& operator=(const HttpClient&) = delete;

  // Returns false only on transport errors (DNS, TLS, stall, ...);
  // HTTP error statuses are a successful request with res.status set.
  bool Perform(const Request& req, Response& res, std::string& error);

  // The outcome of one request of several: what Perform() returns and
  // leaves in 'res' and 'error'.
  struct Result {
    bool ok = false;
    Response response;
    std::string error;
  };

  // Performs all of 'requests' concurrently (a bounded number at a time)
  // and returns their results in the same order. One failing does not stop
  // the others. Blocks until every one has finished or been given up on.
  std::vector<Result> PerformAll(const std::vector<Request>& requests);

private:
  struct Transfer;
  static void Setup(CURL* curl, const Request& req, Transfer& transfer, Response& res);
  static bool Finish(CURL* curl, CURLcode code, const Request& req, Transfer& transfer,
                     Response& res, std::string& error, bool* dates);
  // One try. 'dates' (if given) is set when it failed only because the
  // certificate's dates did not fit this console's clock.
  bool PerformOnce(const Request& req, Response& res, std::string& error, bool* dates);
  // Sets net/clock.h's skew from the server's Date header. Returns whether
  // it changed.
  bool LearnServerTime(const std::string& url);

  CURL* curl_ = nullptr;
  CURLM* multi_ = nullptr; // made by the first PerformAll()
};

// Headers to send with requests for URLs that start with a prefix: what a
// plugin declares for its artwork, for an image server that answers only
// requests carrying, say, its own site as Referer. The prefix must name a
// site ("https://host/..."); a later rule for the same prefix replaces the
// earlier one, and an empty list removes it. Thread safe.
using HeaderList = std::vector<std::pair<std::string, std::string>>;
void SetPrefixHeaders(const std::string& prefix, HeaderList headers);
// The headers of every rule whose prefix 'url' starts with.
HeaderList PrefixHeaders(const std::string& url);

} // namespace net

#endif
