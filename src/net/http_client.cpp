// SPDX-License-Identifier: GPL-3.0-or-later
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <ctime>
#include <memory>
#include <mutex>

#include <strings.h>

#include <openssl/ssl.h>
#include <openssl/x509_vfy.h>

#include "net/cancel.h"
#include "net/clock.h"
#include "net/http_client.h"

namespace net {
  namespace {

    constexpr long kConnectTimeoutMs = 10000;
    // PerformAll(): transfers under way at once, in all and to one host. A
    // console's network stack, and a server's patience, have limits.
    constexpr int kMaxParallel = 8;
    constexpr int kMaxParallelPerHost = 6;

    struct Sink {
      std::string data;
      size_t limit = 0;
      bool truncate = false; // keep what fits rather than fail
      bool overflow = false;
    };

    // Stall detection: curl reports progress about once a second even when
    // nothing moves, so the transfer is aborted once the byte counters
    // have stood still for longer than the limit. Waiting for the server
    // to start answering counts as a stall too, which is the point: a dead
    // server and a dead link look the same from here.
    struct Progress {
      using Clock = std::chrono::steady_clock;
      long stall_ms = 0;
      curl_off_t last_bytes = -1;
      Clock::time_point last_move = Clock::now();
      bool stalled = false;
      bool cancelled = false;
    };

    int OnProgress(void* userdata, curl_off_t dltotal, curl_off_t dlnow,
                   curl_off_t ultotal, curl_off_t ulnow) {
      (void)dltotal;
      (void)ultotal;
      Progress* p = static_cast<Progress*>(userdata);
      // Whoever asked has stopped waiting (net/cancel.h): so do we.
      if (Cancelled()) {
        p->cancelled = true;
        return 1;
      }
      const curl_off_t bytes = dlnow + ulnow;
      const Progress::Clock::time_point now = Progress::Clock::now();
      if (bytes != p->last_bytes) {
        p->last_bytes = bytes;
        p->last_move = now;
        return 0;
      }
      const auto idle = std::chrono::duration_cast<std::chrono::milliseconds>(
        now - p->last_move).count();
      if (p->stall_ms > 0 && idle > p->stall_ms) {
        p->stalled = true;
        return 1; // aborts with CURLE_ABORTED_BY_CALLBACK
      }
      return 0;
    }

    size_t WriteBody(char* ptr, size_t size, size_t nmemb, void* userdata) {
      Sink* sink = static_cast<Sink*>(userdata);
      const size_t n = size * nmemb;
      if (sink->data.size() + n > sink->limit) {
        sink->overflow = true;
        if (sink->truncate)
          sink->data.append(ptr, sink->limit - sink->data.size());
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

    // Certificates are judged by the servers' clock (net/clock.h): by a
    // console clock days off, valid ones look expired or not valid yet.
    CURLcode OnSslContext(CURL*, void* ssl_ctx, void*) {
      const int64_t skew = netclock::skew.load();
      if (skew < -netclock::kToleranceSec || skew > netclock::kToleranceSec)
        X509_VERIFY_PARAM_set_time(SSL_CTX_get0_param((SSL_CTX*)ssl_ctx),
                                   (time_t)netclock::Now());
      return CURLE_OK;
    }

    bool IsHttps(const std::string& url) {
      return url.size() > 8 && ::strncasecmp(url.c_str(), "https://", 8) == 0;
    }

    // Goes by a response's Date header, when there is one. Returns whether
    // that changed the clock.
    bool LearnSkew(const std::map<std::string, std::string>& headers) {
      const auto date = headers.find("date");
      if (date == headers.end())
        return false;
      const time_t server = ::curl_getdate(date->second.c_str(), nullptr);
      if (server <= 0)
        return false;
      int64_t skew = (int64_t)server - (int64_t)std::time(nullptr);
      if (skew >= -netclock::kToleranceSec && skew <= netclock::kToleranceSec)
        skew = 0;
      const int64_t was = netclock::skew.exchange(skew);
      if (skew != 0 && (was - skew > netclock::kToleranceSec ||
                        skew - was > netclock::kToleranceSec))
        std::fprintf(stderr, "http: this clock is %lld s %s the servers'; "
                     "checking certificates by theirs\n",
                     (long long)(skew < 0 ? -skew : skew),
                     skew > 0 ? "behind" : "ahead of");
      return was != skew;
    }

  } // namespace

  HttpClient::HttpClient() {
    std::call_once(g_curl_init, [] { ::curl_global_init(CURL_GLOBAL_DEFAULT); });
    curl_ = ::curl_easy_init();
  }

  HttpClient::~HttpClient() {
    if (multi_)
      ::curl_multi_cleanup(multi_);
    if (curl_)
      ::curl_easy_cleanup(curl_);
  }

  // A certificate turned down only for its dates may be the console's
  // clock's fault, not the server's. If the server's own Date header (read
  // without checking the certificate: it is used for nothing else) says the
  // clock is off, the request is tried again by the server's time.
  bool HttpClient::Perform(const Request& req, Response& res,
                           std::string& error) {
    bool dates = false;
    if (PerformOnce(req, res, error, &dates))
      return true;
    if (!dates || !LearnServerTime(req.url))
      return false;
    res = Response{};
    error.clear();
    return PerformOnce(req, res, error, nullptr);
  }

  bool HttpClient::LearnServerTime(const std::string& url) {
    ::curl_easy_reset(curl_);
    std::map<std::string, std::string> headers;
    ::curl_easy_setopt(curl_, CURLOPT_URL, url.c_str());
    ::curl_easy_setopt(curl_, CURLOPT_NOBODY, 1L);
    ::curl_easy_setopt(curl_, CURLOPT_HEADERFUNCTION, WriteHeader);
    ::curl_easy_setopt(curl_, CURLOPT_HEADERDATA, &headers);
    ::curl_easy_setopt(curl_, CURLOPT_USERAGENT, "jtplay");
    ::curl_easy_setopt(curl_, CURLOPT_SSL_VERIFYPEER, 0L);
    ::curl_easy_setopt(curl_, CURLOPT_SSL_VERIFYHOST, 0L);
    ::curl_easy_setopt(curl_, CURLOPT_TIMEOUT_MS, kConnectTimeoutMs);
    ::curl_easy_setopt(curl_, CURLOPT_NOSIGNAL, 1L);
    // Whatever was cached was opened without checking anything.
    ::curl_easy_setopt(curl_, CURLOPT_FORBID_REUSE, 1L);
    ::curl_easy_setopt(curl_, CURLOPT_FRESH_CONNECT, 1L);
    ::curl_easy_perform(curl_);
    return LearnSkew(headers);
  }

  // What one transfer needs to stay alive while libcurl runs it.
  struct HttpClient::Transfer {
    Sink sink;
    Progress progress;
    char message[CURL_ERROR_SIZE] = {0};
    struct curl_slist* headers = nullptr;

    Transfer() = default;
    ~Transfer() {
      if (headers)
        ::curl_slist_free_all(headers);
    }
    Transfer(const Transfer&) = delete;
    Transfer& operator=(const Transfer&) = delete;
  };

  void HttpClient::Setup(CURL* curl, const Request& req, Transfer& t, Response& res) {
    t.sink.limit = req.max_bytes;
    t.sink.truncate = req.truncate;
    t.progress.stall_ms = req.stall_timeout_ms;

    ::curl_easy_setopt(curl, CURLOPT_URL, req.url.c_str());
    ::curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, t.message);
    ::curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteBody);
    ::curl_easy_setopt(curl, CURLOPT_WRITEDATA, &t.sink);
    ::curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, WriteHeader);
    ::curl_easy_setopt(curl, CURLOPT_HEADERDATA, &res.headers);
    ::curl_easy_setopt(curl, CURLOPT_USERAGENT, "jtplay");
    ::curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, ""); // gzip if built in
    ::curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, req.follow_redirects ? 1L : 0L);
    ::curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 5L);
    ::curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, req.deadline_ms);
    ::curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS,
                       std::min(kConnectTimeoutMs,
                                req.stall_timeout_ms > 0 ? req.stall_timeout_ms
                                                         : kConnectTimeoutMs));
    ::curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, OnProgress);
    ::curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &t.progress);
    ::curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    // The worker thread must not be hit by libcurl's signal-based timeouts.
    ::curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);

    // A client lives as long as the process (or its plugin), and the
    // network under it can change in that time: a new DHCP lease, a Wi-Fi
    // reconnect, a NAT entry that timed out overnight. Neither a name
    // lookup nor an idle connection is trusted for long, whatever this
    // libcurl's defaults are.
    ::curl_easy_setopt(curl, CURLOPT_DNS_CACHE_TIMEOUT, 60L);
#if LIBCURL_VERSION_NUM >= 0x074100
    ::curl_easy_setopt(curl, CURLOPT_MAXAGE_CONN, 30L); // idle seconds
#endif
#if LIBCURL_VERSION_NUM >= 0x075000
    ::curl_easy_setopt(curl, CURLOPT_MAXLIFETIME_CONN, 300L);
#endif

    // OpenSSL only; others say CURLE_NOT_BUILT_IN and check by the console.
    ::curl_easy_setopt(curl, CURLOPT_SSL_CTX_FUNCTION, OnSslContext);

    // Neither a plugin nor an <img src> has any business opening file:// or
    // scp:// URLs.
#if LIBCURL_VERSION_NUM >= 0x075500
    ::curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "http,https");
    ::curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS_STR, "http,https");
#else
    ::curl_easy_setopt(curl, CURLOPT_PROTOCOLS, (long)(CURLPROTO_HTTP | CURLPROTO_HTTPS));
    ::curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS, (long)(CURLPROTO_HTTP | CURLPROTO_HTTPS));
#endif

    // Builds without a baked-in CA bundle (the PS5 toolchain among them)
    // need to be told where the certificates live.
    for (const char* name : {"SSL_CERT_FILE", "CURL_CA_BUNDLE"}) {
      if (const char* path = ::getenv(name)) {
        if (*path) {
          ::curl_easy_setopt(curl, CURLOPT_CAINFO, path);
          break;
        }
      }
    }

    if (req.method == "GET") {
      ::curl_easy_setopt(curl, CURLOPT_HTTPGET, 1L);
    } else if (req.method == "HEAD") {
      ::curl_easy_setopt(curl, CURLOPT_NOBODY, 1L);
    } else {
      ::curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, req.method.c_str());
    }
    if (req.has_body) {
      ::curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE_LARGE, (curl_off_t)req.body.size());
      ::curl_easy_setopt(curl, CURLOPT_COPYPOSTFIELDS, req.body.data());
    }

    for (const auto& header : req.headers)
      t.headers = ::curl_slist_append(t.headers, (header.first + ": " + header.second).c_str());
    if (t.headers)
      ::curl_easy_setopt(curl, CURLOPT_HTTPHEADER, t.headers);
  }

  bool HttpClient::Finish(CURL* curl, CURLcode code, const Request& req, Transfer& t,
                          Response& res, std::string& error, bool* dates) {
    // A body cut short on purpose is a result, not a failure.
    const bool cut = code == CURLE_WRITE_ERROR && t.sink.overflow && req.truncate;
    if (code != CURLE_OK && !cut) {
      if (t.progress.cancelled || Cancelled())
        error = kCancelledError;
      else if (t.sink.overflow)
        error = "http: response larger than " +
          std::to_string(req.max_bytes / (1024 * 1024)) + " MiB";
      else if (t.progress.stalled)
        error = "http: no data from the server for " +
          std::to_string(req.stall_timeout_ms / 1000) + " s";
      else
        error = std::string("http: ") + (t.message[0] ? t.message : ::curl_easy_strerror(code));
      if (dates && code == CURLE_PEER_FAILED_VERIFICATION) {
        long verify = 0;
        ::curl_easy_getinfo(curl, CURLINFO_SSL_VERIFYRESULT, &verify);
        *dates = verify == X509_V_ERR_CERT_NOT_YET_VALID ||
          verify == X509_V_ERR_CERT_HAS_EXPIRED;
      }
      return false;
    }

    ::curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &res.status);
    const char* final_url = nullptr;
    ::curl_easy_getinfo(curl, CURLINFO_EFFECTIVE_URL, &final_url);
    res.url = final_url ? final_url : req.url;
    res.body = std::move(t.sink.data);
    res.truncated = cut;
    // A certificate that checked out vouches for its server's clock.
    if (IsHttps(res.url))
      LearnSkew(res.headers);
    return true;
  }

  bool HttpClient::PerformOnce(const Request& req, Response& res,
                               std::string& error, bool* dates) {
    if (!curl_) {
      error = "http: curl unavailable";
      return false;
    }
    if (Cancelled()) {
      error = kCancelledError;
      return false;
    }
    ::curl_easy_reset(curl_); // keeps the connection cache, drops old options

    Transfer transfer;
    Setup(curl_, req, transfer, res);
    const CURLcode code = ::curl_easy_perform(curl_);
    return Finish(curl_, code, req, transfer, res, error, dates);
  }

  // Several requests at once, on libcurl's multi interface: the waiting is
  // shared, so twenty small lookups cost about as long as the slowest one
  // rather than the sum of them all.
  std::vector<HttpClient::Result> HttpClient::PerformAll(const std::vector<Request>& requests) {
    std::vector<Result> results(requests.size());
    if (requests.empty())
      return results;

    if (!multi_)
      multi_ = ::curl_multi_init();
    if (!multi_) {
      for (Result& r : results)
        r.error = "http: curl unavailable";
      return results;
    }
    ::curl_multi_setopt(multi_, CURLMOPT_MAX_TOTAL_CONNECTIONS, (long)kMaxParallel);
    ::curl_multi_setopt(multi_, CURLMOPT_MAX_HOST_CONNECTIONS, (long)kMaxParallelPerHost);
    ::curl_multi_setopt(multi_, CURLMOPT_PIPELINING, (long)CURLPIPE_MULTIPLEX);

    struct Job {
      CURL* easy = nullptr;
      std::unique_ptr<Transfer> transfer;
      bool dates = false;
    };
    std::vector<Job> jobs(requests.size());
    for (size_t i = 0; i < requests.size(); i++) {
      Job& job = jobs[i];
      job.easy = ::curl_easy_init();
      if (!job.easy) {
        results[i].error = "http: curl unavailable";
        continue;
      }
      job.transfer = std::make_unique<Transfer>();
      Setup(job.easy, requests[i], *job.transfer, results[i].response);
      ::curl_easy_setopt(job.easy, CURLOPT_PRIVATE, (char*)(uintptr_t)i);
      ::curl_multi_add_handle(multi_, job.easy);
    }

    int running = 0;
    do {
      const CURLMcode mc = ::curl_multi_perform(multi_, &running);
      if (mc != CURLM_OK && mc != CURLM_CALL_MULTI_PERFORM)
        break;

      int queued = 0;
      while (CURLMsg* msg = ::curl_multi_info_read(multi_, &queued)) {
        if (msg->msg != CURLMSG_DONE)
          continue;
        char* tag = nullptr;
        ::curl_easy_getinfo(msg->easy_handle, CURLINFO_PRIVATE, &tag);
        const size_t i = (size_t)(uintptr_t)tag;
        if (i >= jobs.size() || !jobs[i].transfer)
          continue;
        results[i].ok = Finish(msg->easy_handle, msg->data.result, requests[i],
                               *jobs[i].transfer, results[i].response, results[i].error,
                               &jobs[i].dates);
        jobs[i].transfer.reset();
      }

      // Each transfer's progress callback notices a cancel within about a
      // second; this is for the ones still waiting for a connection.
      if (Cancelled())
        break;
      if (running) {
#if LIBCURL_VERSION_NUM >= 0x074200
        ::curl_multi_poll(multi_, nullptr, 0, 200, nullptr);
#else
        ::curl_multi_wait(multi_, nullptr, 0, 200, nullptr);
#endif
      }
    } while (running);

    for (size_t i = 0; i < jobs.size(); i++) {
      Job& job = jobs[i];
      if (!job.easy)
        continue;
      ::curl_multi_remove_handle(multi_, job.easy);
      ::curl_easy_cleanup(job.easy);
      if (job.transfer) { // never finished: cancelled, or the multi gave up
        results[i].error = Cancelled() ? kCancelledError : "http: transfer abandoned";
        job.transfer.reset();
      }
    }

    // Certificates turned down for their dates: the console's clock may be
    // what is wrong (see Perform), so those go once more, one at a time.
    for (size_t i = 0; i < jobs.size(); i++) {
      if (results[i].ok || !jobs[i].dates || Cancelled())
        continue;
      if (!LearnServerTime(requests[i].url))
        break; // the clock is as right as it gets; the rest fail the same way
      results[i] = Result{};
      results[i].ok = PerformOnce(requests[i], results[i].response, results[i].error, nullptr);
    }
    return results;
  }

} // namespace net
