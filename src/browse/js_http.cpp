// SPDX-License-Identifier: GPL-3.0-or-later
//
// The 'http' global available to plugin scripts. The transport itself is
// net::HttpClient (see net/http_client.h), shared with the artwork cache;
// this is only the QuickJS binding on top of it. Requests are blocking:
// plugins run on the app's worker threads, which is already where the DLNA
// source does its blocking I/O. (The prelude wraps what is installed here
// with a few conveniences; see js_prelude.h.)
#include <algorithm>
#include <cctype>
#include <chrono>
#include <vector>

#include "browse/js_plugin.h"
#include "browse/js_util.h"
#include "net/cancel.h"

namespace browse {
namespace {

using namespace js;

// A plugin listing is metadata, not media; anything this large is a
// mistake, and the body is held outside the JS heap so the runtime's
// own memory limit would not catch it.
constexpr size_t kMaxResponseBytes = 8u << 20;
constexpr long kMaxTimeoutMs = 60000;
// http.all(): requests in one call, and what their bodies may add up to
// (they are all held at once, outside the JS heap, before any is handed
// over).
constexpr int64_t kMaxBatch = 64;
constexpr size_t kMaxBatchBytes = 24u << 20;

JsPlugin* PluginFor(JSContext* ctx) {
  return static_cast<JsPlugin*>(JS_GetContextOpaque(ctx));
}

// What a request asks of the response, besides the request itself.
struct Call {
  net::HttpClient::Request req;
  bool binary = false;
};

// Reads the options shared by request(), get() and post():
//   headers, timeout, redirect, maxBytes, binary
bool ReadOptions(JSContext* ctx, JSValue options, Call& call) {
  if (JS_IsUndefined(options) || JS_IsNull(options))
    return true;
  if (!JS_IsObject(options)) {
    JS_ThrowTypeError(ctx, "http: options must be an object");
    return false;
  }
  net::HttpClient::Request& req = call.req;

  Ref headers(ctx, JS_GetPropertyStr(ctx, options, "headers"));
  if (JS_IsObject(headers.get())) {
    const bool ok = ForEachProperty(ctx, headers.get(), [&](const char* name, JSValue value) {
      std::string text;
      if (!StringArg(ctx, value, text))
        return false;
      req.headers.emplace_back(name, text);
      return true;
    });
    if (!ok)
      return false;
  }

  Ref timeout(ctx, JS_GetPropertyStr(ctx, options, "timeout"));
  if (JS_IsNumber(timeout.get())) {
    double seconds = 0.0;
    JS_ToFloat64(ctx, &seconds, timeout.get());
    if (seconds > 0.0)
      req.stall_timeout_ms = std::min((long)(seconds * 1000.0), kMaxTimeoutMs);
  }

  req.follow_redirects = FieldBool(ctx, options, "redirect", req.follow_redirects);

  // maxBytes: read at most this much and hand back what came, with
  // 'truncated' set, instead of failing on a larger response.
  Ref max_bytes(ctx, JS_GetPropertyStr(ctx, options, "maxBytes"));
  if (JS_IsNumber(max_bytes.get())) {
    double n = 0.0;
    JS_ToFloat64(ctx, &n, max_bytes.get());
    if (n >= 1.0) {
      req.max_bytes = n < (double)kMaxResponseBytes ? (size_t)n : kMaxResponseBytes;
      req.truncate = true;
    }
  }

  call.binary = FieldBool(ctx, options, "binary", call.binary);
  return true;
}

// Reads { method, url, body, ...options }, as request() and all() take it.
bool ReadRequestObject(JSContext* ctx, JSValue object, Call& call) {
  net::HttpClient::Request& req = call.req;

  Ref url(ctx, JS_GetPropertyStr(ctx, object, "url"));
  if (!JS_IsString(url.get()) || !StringArg(ctx, url.get(), req.url) || req.url.empty()) {
    JS_ThrowTypeError(ctx, "http: 'url' is required");
    return false;
  }

  Ref method(ctx, JS_GetPropertyStr(ctx, object, "method"));
  if (JS_IsString(method.get()) && !StringArg(ctx, method.get(), req.method))
    return false;
  std::transform(req.method.begin(), req.method.end(), req.method.begin(),
                 [](unsigned char c) { return std::toupper(c); });

  Ref body(ctx, JS_GetPropertyStr(ctx, object, "body"));
  if (!JS_IsUndefined(body.get()) && !JS_IsNull(body.get())) {
    if (!StringArg(ctx, body.get(), req.body))
      return false;
    req.has_body = true;
  }
  return ReadOptions(ctx, object, call);
}

// Length of 's' without a UTF-8 sequence cut off at the end, which a
// truncated body may have.
size_t Utf8Prefix(const std::string& s) {
  size_t n = s.size();
  size_t back = 0;
  while (back < 4 && back < n && ((unsigned char)s[n - 1 - back] & 0xC0) == 0x80)
    back++;
  if (back == n || back == 4)
    return n; // not UTF-8 we can reason about; leave it alone
  const unsigned char lead = (unsigned char)s[n - 1 - back];
  const size_t want = lead >= 0xF0 ? 4 : lead >= 0xE0 ? 3 : lead >= 0xC0 ? 2 : 1;
  return want > back + 1 ? n - 1 - back : n;
}

// { ok, status, url, headers, body, truncated }
JSValue BuildResponse(JSContext* ctx, const net::HttpClient::Response& res, bool binary) {
  JSValue out = JS_NewObject(ctx);
  JS_SetPropertyStr(ctx, out, "status", JS_NewInt32(ctx, (int)res.status));
  JS_SetPropertyStr(ctx, out, "ok", JS_NewBool(ctx, res.status >= 200 && res.status < 300));
  JS_SetPropertyStr(ctx, out, "url", NewString(ctx, res.url));

  JSValue headers = JS_NewObject(ctx);
  for (const auto& header : res.headers)
    JS_SetPropertyStr(ctx, headers, header.first.c_str(), NewString(ctx, header.second));
  JS_SetPropertyStr(ctx, out, "headers", headers);
  JS_SetPropertyStr(ctx, out, "truncated", JS_NewBool(ctx, res.truncated));

  // A text body goes through JS_NewStringLen, which expects UTF-8;
  // 'binary: true' hands back an ArrayBuffer instead.
  if (binary)
    JS_SetPropertyStr(ctx, out, "body",
                      JS_NewArrayBufferCopy(ctx, (const uint8_t*)res.body.data(),
                                            res.body.size()));
  else
    JS_SetPropertyStr(ctx, out, "body",
                      JS_NewStringLen(ctx, res.body.data(),
                                      res.truncated ? Utf8Prefix(res.body) : res.body.size()));
  return out;
}

// all(): a request that never got an answer, in the shape of one, so the
// rest of the batch is not lost with it: { ok: false, status: 0, error }.
JSValue BuildFailure(JSContext* ctx, const std::string& url, const std::string& error) {
  JSValue out = JS_NewObject(ctx);
  JS_SetPropertyStr(ctx, out, "status", JS_NewInt32(ctx, 0));
  JS_SetPropertyStr(ctx, out, "ok", JS_NewBool(ctx, false));
  JS_SetPropertyStr(ctx, out, "url", NewString(ctx, url));
  JS_SetPropertyStr(ctx, out, "headers", JS_NewObject(ctx));
  JS_SetPropertyStr(ctx, out, "truncated", JS_NewBool(ctx, false));
  JS_SetPropertyStr(ctx, out, "body", JS_NewString(ctx, ""));
  JS_SetPropertyStr(ctx, out, "error", NewString(ctx, error));
  return out;
}

// JsEnvironment::url_rewrites, applied to a request about to go out.
void Rewrite(const JsPlugin* plugin, net::HttpClient::Request& req) {
  for (const auto& rule : plugin->Environment().url_rewrites) {
    if (req.url.compare(0, rule.first.size(), rule.first) == 0) {
      req.url = rule.second + req.url.substr(rule.first.size());
      return;
    }
  }
}

// Runs 'work' (a blocking transfer) without charging its time to the
// script watchdog: waiting on a slow server is not a runaway loop.
template <typename Work>
auto Unwatched(JsPlugin* plugin, Work&& work) {
  const auto started = std::chrono::steady_clock::now();
  auto result = work();
  plugin->ExtendDeadline(std::chrono::steady_clock::now() - started);
  return result;
}

JSValue Perform(JSContext* ctx, Call& call) {
  JsPlugin* plugin = PluginFor(ctx);
  net::HttpClient* client = plugin ? plugin->Http() : nullptr;
  if (!client)
    return ThrowError(ctx, "http: client unavailable");

  call.req.max_bytes = std::min(call.req.max_bytes, kMaxResponseBytes);
  Rewrite(plugin, call.req);

  net::HttpClient::Response res;
  std::string error;
  const bool ok = Unwatched(plugin, [&] { return client->Perform(call.req, res, error); });
  if (!ok)
    return ThrowError(ctx, error);
  return BuildResponse(ctx, res, call.binary);
}

// http.request({ method, url, headers, body, timeout, redirect, binary })
JSValue HttpRequest(JSContext* ctx, JSValue, int argc, JSValue* argv) {
  if (argc < 1 || !JS_IsObject(argv[0]))
    return JS_ThrowTypeError(ctx, "http.request: expected an options object");
  Call call;
  if (!ReadRequestObject(ctx, argv[0], call))
    return JS_EXCEPTION;
  return Perform(ctx, call);
}

// http.get(url, options?)
JSValue HttpGet(JSContext* ctx, JSValue, int argc, JSValue* argv) {
  Call call;
  if (argc < 1 || !JS_IsString(argv[0]) || !StringArg(ctx, argv[0], call.req.url))
    return JS_ThrowTypeError(ctx, "http.get: expected a url string");
  if (argc > 1 && !ReadOptions(ctx, argv[1], call))
    return JS_EXCEPTION;
  return Perform(ctx, call);
}

// http.post(url, body, options?)
JSValue HttpPost(JSContext* ctx, JSValue, int argc, JSValue* argv) {
  Call call;
  call.req.method = "POST";
  if (argc < 1 || !JS_IsString(argv[0]) || !StringArg(ctx, argv[0], call.req.url))
    return JS_ThrowTypeError(ctx, "http.post: expected a url string");
  if (argc > 1 && !JS_IsUndefined(argv[1]) && !JS_IsNull(argv[1])) {
    if (!StringArg(ctx, argv[1], call.req.body))
      return JS_EXCEPTION;
    call.req.has_body = true;
  }
  if (argc > 2 && !ReadOptions(ctx, argv[2], call))
    return JS_EXCEPTION;
  return Perform(ctx, call);
}

// http.all([url | { url, method, headers, body, ... }, ...])
//
// Performs the requests concurrently and returns their responses in the
// same order. Unlike the calls above, a request that fails in transport
// does not throw: its place holds { ok: false, status: 0, error }, so one
// dead server does not cost the answers of the others.
JSValue HttpAll(JSContext* ctx, JSValue, int argc, JSValue* argv) {
  JsPlugin* plugin = PluginFor(ctx);
  net::HttpClient* client = plugin ? plugin->Http() : nullptr;
  if (!client)
    return ThrowError(ctx, "http: client unavailable");

  int64_t n = 0;
  if (argc < 1 || !ArrayLength(ctx, argv[0], n))
    return JS_ThrowTypeError(ctx, "http.all: expected an array of requests");
  if (n > kMaxBatch)
    return JS_ThrowRangeError(ctx, "http.all: at most %d requests at a time", (int)kMaxBatch);

  std::vector<Call> calls((size_t)n);
  std::vector<net::HttpClient::Request> requests;
  requests.reserve((size_t)n);
  for (int64_t i = 0; i < n; i++) {
    Ref item(ctx, JS_GetPropertyUint32(ctx, argv[0], (uint32_t)i));
    Call& call = calls[(size_t)i];
    if (JS_IsString(item.get())) {
      if (!StringArg(ctx, item.get(), call.req.url))
        return JS_EXCEPTION;
    } else if (JS_IsObject(item.get())) {
      if (!ReadRequestObject(ctx, item.get(), call))
        return JS_EXCEPTION;
    } else {
      return JS_ThrowTypeError(ctx, "http.all: request %d is neither a url nor an object",
                               (int)i + 1);
    }
    // A fair share of what the batch may hold, so one large body cannot
    // crowd the rest out.
    call.req.max_bytes = std::min({call.req.max_bytes, kMaxResponseBytes,
                                   kMaxBatchBytes / (size_t)std::max<int64_t>(n, 1)});
    Rewrite(plugin, call.req);
    requests.push_back(call.req);
  }

  const std::vector<net::HttpClient::Result> results =
    Unwatched(plugin, [&] { return client->PerformAll(requests); });

  // Given up on altogether: nobody is waiting for a partial answer.
  for (const net::HttpClient::Result& r : results)
    if (!r.ok && r.error == net::kCancelledError)
      return ThrowError(ctx, r.error);

  JSValue out = JS_NewArray(ctx);
  for (size_t i = 0; i < results.size(); i++) {
    const net::HttpClient::Result& r = results[i];
    JS_SetPropertyUint32(ctx, out, (uint32_t)i,
                         r.ok ? BuildResponse(ctx, r.response, calls[i].binary)
                              : BuildFailure(ctx, requests[i].url, r.error));
  }
  return out;
}

} // namespace

void InstallHttp(JSContext* ctx) {
  Ref global(ctx, JS_GetGlobalObject(ctx));
  JSValue http = JS_NewObject(ctx);
  JS_SetPropertyStr(ctx, http, "request", JS_NewCFunction(ctx, HttpRequest, "request", 1));
  JS_SetPropertyStr(ctx, http, "get", JS_NewCFunction(ctx, HttpGet, "get", 2));
  JS_SetPropertyStr(ctx, http, "post", JS_NewCFunction(ctx, HttpPost, "post", 3));
  JS_SetPropertyStr(ctx, http, "all", JS_NewCFunction(ctx, HttpAll, "all", 1));
  JS_SetPropertyStr(ctx, global.get(), "http", http);
}

} // namespace browse
