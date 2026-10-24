// SPDX-License-Identifier: GPL-3.0-or-later
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <mutex>

#include "net/http_client.h"

namespace net {
  namespace {

    constexpr long kConnectTimeoutMs = 10000;

    struct Sink {
      std::string data;
      size_t limit = 0;
      bool overflow = false;
    };

    size_t WriteBody(char* ptr, size_t size, size_t nmemb, void* userdata) {
      Sink* sink = static_cast<Sink*>(userdata);
      const size_t n = size * nmemb;
      if (sink->data.size() + n > sink->limit) {
	sink->overflow = true;
	return 0; // aborts the transfer with CURLE_WRITE_ERROR
      }
      sink->data.append(ptr, n);
      return n;
    }

    size_t WriteHeader(char* ptr, size_t size, size_t nmemb, void* userdata) {
      auto* headers =
	static_cast<std::map<std::string, std::string>*>(userdata);
      const size_t n = size * nmemb;
      std::string line(ptr, n);
      while (!line.empty() && (line.back() == '\r' || line.back() == '\n'))
	line.pop_back();

      // A new status line means a redirect; the headers seen so far belong
      // to the response we are not returning.
      if (line.compare(0, 5, "HTTP/") == 0) {
	headers->clear();
	return n;
      }
      const size_t colon = line.find(':');
      if (colon == std::string::npos)
	return n;

      std::string key = line.substr(0, colon);
      std::transform(key.begin(), key.end(), key.begin(),
		     [](unsigned char c) { return std::tolower(c); });
      size_t begin = line.find_first_not_of(" \t", colon + 1);
      std::string value =
	begin == std::string::npos ? std::string() : line.substr(begin);

      auto it = headers->find(key);
      if (it == headers->end())
	(*headers)[key] = std::move(value);
      else
	it->second += ", " + value; // repeated header (Set-Cookie, Link, ...)
      return n;
    }

    std::once_flag g_curl_init;

  } // namespace

  HttpClient::HttpClient() {
    std::call_once(g_curl_init, [] { ::curl_global_init(CURL_GLOBAL_DEFAULT); });
    curl_ = ::curl_easy_init();
  }

  HttpClient::~HttpClient() {
    if (curl_)
      ::curl_easy_cleanup(curl_);
  }

  bool HttpClient::Perform(const Request& req, Response& res,
			   std::string& error) {
    if (!curl_) {
      error = "http: curl unavailable";
      return false;
    }
    ::curl_easy_reset(curl_); // keeps the connection cache, drops old options

    Sink sink;
    sink.limit = req.max_bytes;
    char message[CURL_ERROR_SIZE] = {0};

    ::curl_easy_setopt(curl_, CURLOPT_URL, req.url.c_str());
    ::curl_easy_setopt(curl_, CURLOPT_ERRORBUFFER, message);
    ::curl_easy_setopt(curl_, CURLOPT_WRITEFUNCTION, WriteBody);
    ::curl_easy_setopt(curl_, CURLOPT_WRITEDATA, &sink);
    ::curl_easy_setopt(curl_, CURLOPT_HEADERFUNCTION, WriteHeader);
    ::curl_easy_setopt(curl_, CURLOPT_HEADERDATA, &res.headers);
    ::curl_easy_setopt(curl_, CURLOPT_USERAGENT, "jtplay");
    ::curl_easy_setopt(curl_, CURLOPT_ACCEPT_ENCODING, ""); // gzip if built in
    ::curl_easy_setopt(curl_, CURLOPT_FOLLOWLOCATION,
		       req.follow_redirects ? 1L : 0L);
    ::curl_easy_setopt(curl_, CURLOPT_MAXREDIRS, 5L);
    ::curl_easy_setopt(curl_, CURLOPT_TIMEOUT_MS, req.timeout_ms);
    ::curl_easy_setopt(curl_, CURLOPT_CONNECTTIMEOUT_MS, kConnectTimeoutMs);
    // The worker thread must not be hit by libcurl's signal-based timeouts.
    ::curl_easy_setopt(curl_, CURLOPT_NOSIGNAL, 1L);

    // Neither a plugin nor an <img src> has any business opening file:// or
    // scp:// URLs.
#if LIBCURL_VERSION_NUM >= 0x075500
    ::curl_easy_setopt(curl_, CURLOPT_PROTOCOLS_STR, "http,https");
    ::curl_easy_setopt(curl_, CURLOPT_REDIR_PROTOCOLS_STR, "http,https");
#else
    ::curl_easy_setopt(curl_, CURLOPT_PROTOCOLS,
		       (long)(CURLPROTO_HTTP | CURLPROTO_HTTPS));
    ::curl_easy_setopt(curl_, CURLOPT_REDIR_PROTOCOLS,
		       (long)(CURLPROTO_HTTP | CURLPROTO_HTTPS));
#endif

    // Builds without a baked-in CA bundle (the PS5 toolchain among them)
    // need to be told where the certificates live.
    for (const char* name : {"SSL_CERT_FILE", "CURL_CA_BUNDLE"}) {
      if (const char* path = ::getenv(name)) {
	if (*path) {
	  ::curl_easy_setopt(curl_, CURLOPT_CAINFO, path);
	  break;
	}
      }
    }

    if (req.method == "GET") {
      ::curl_easy_setopt(curl_, CURLOPT_HTTPGET, 1L);
    } else if (req.method == "HEAD") {
      ::curl_easy_setopt(curl_, CURLOPT_NOBODY, 1L);
    } else {
      ::curl_easy_setopt(curl_, CURLOPT_CUSTOMREQUEST, req.method.c_str());
    }
    if (req.has_body) {
      ::curl_easy_setopt(curl_, CURLOPT_POSTFIELDSIZE_LARGE,
			 (curl_off_t)req.body.size());
      ::curl_easy_setopt(curl_, CURLOPT_COPYPOSTFIELDS, req.body.data());
    }

    struct curl_slist* headers = nullptr;
    for (const auto& header : req.headers)
      headers = ::curl_slist_append(headers,
				    (header.first + ": " + header.second).c_str());
    if (headers)
      ::curl_easy_setopt(curl_, CURLOPT_HTTPHEADER, headers);

    const CURLcode code = ::curl_easy_perform(curl_);
    if (headers)
      ::curl_slist_free_all(headers);

    if (code != CURLE_OK) {
      if (sink.overflow)
	error = "http: response larger than " +
	  std::to_string(req.max_bytes / (1024 * 1024)) + " MiB";
      else
	error = std::string("http: ") +
	  (message[0] ? message : ::curl_easy_strerror(code));
      return false;
    }

    ::curl_easy_getinfo(curl_, CURLINFO_RESPONSE_CODE, &res.status);
    const char* final_url = nullptr;
    ::curl_easy_getinfo(curl_, CURLINFO_EFFECTIVE_URL, &final_url);
    res.url = final_url ? final_url : req.url;
    res.body = std::move(sink.data);
    return true;
  }

} // namespace net
