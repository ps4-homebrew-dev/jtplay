// SPDX-License-Identifier: GPL-3.0-or-later
//
// One blocking HTTP client on top of libcurl: https, redirects, content
// encodings and CA-bundle discovery, in one place. The plugin 'http' global
// and the artwork cache both use it, so anything a plugin can reach is also
// something the UI can display.
//
// (upnp/http.h is a different animal on purpose: a hand-rolled plain-http
// client for SOAP control endpoints on the LAN. Nothing outside src/upnp
// should be using it.)
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
    long timeout_ms = 15000;

    // The body is held outside any managed heap, so every caller states how
    // much it is willing to hold. Exceeding it aborts the transfer.
    size_t max_bytes = 8u << 20;
  };

  struct Response {
    long status = 0;
    std::string url;                            // after redirects
    std::map<std::string, std::string> headers; // keys lower-cased
    std::string body;
  };

  HttpClient();
  ~HttpClient();

  HttpClient(const HttpClient&) = delete;
  HttpClient& operator=(const HttpClient&) = delete;

  // Returns false only on transport errors (DNS, TLS, timeout, ...);
  // HTTP error statuses are a successful request with res.status set.
  bool Perform(const Request& req, Response& res, std::string& error);

private:
  CURL* curl_ = nullptr;
};

} // namespace net

#endif
