// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <cctype>
#include <chrono>

#include "browse/js_http.h"
#include "browse/js_source.h"

namespace browse {
  namespace {

    // A plugin listing is metadata, not media; anything this large is a
    // mistake, and the body is held outside the JS heap so the runtime's
    // own memory limit would not catch it.
    constexpr size_t kMaxResponseBytes = 8u << 20;
    constexpr long kMaxTimeoutMs = 60000;

    JsPlugin* PluginFor(JSContext* ctx) {
      return static_cast<JsPlugin*>(JS_GetContextOpaque(ctx));
    }

    // Throws a plain Error, the way a plugin author expects to catch it.
    JSValue ThrowError(JSContext* ctx, const std::string& message) {
      JSValue error = JS_NewError(ctx);
      JS_SetPropertyStr(ctx, error, "message",
			JS_NewStringLen(ctx, message.data(), message.size()));
      return JS_Throw(ctx, error);
    }

    bool StringArg(JSContext* ctx, JSValue v, std::string& out) {
      size_t len = 0;
      const char* s = JS_ToCStringLen(ctx, &len, v);
      if (!s)
	return false;
      out.assign(s, len);
      JS_FreeCString(ctx, s);
      return true;
    }

    // Copies the {name: value} object at 'headers' into the request.
    bool ReadHeaders(JSContext* ctx, JSValue headers,
		     net::HttpClient::Request& req) {
      JSPropertyEnum* props = nullptr;
      uint32_t count = 0;
      if (JS_GetOwnPropertyNames(ctx, &props, &count, headers,
				 JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY) < 0)
	return false;

      bool ok = true;
      for (uint32_t i = 0; i < count && ok; i++) {
	const char* name = JS_AtomToCString(ctx, props[i].atom);
	JSValue value = JS_GetProperty(ctx, headers, props[i].atom);
	std::string text;
	if (!name || JS_IsException(value) || !StringArg(ctx, value, text))
	  ok = false;
	else
	  req.headers.emplace_back(name, text);
	JS_FreeValue(ctx, value);
	if (name)
	  JS_FreeCString(ctx, name);
      }
      // JS_FreePropertyEnum() only exists in newer QuickJS forks; freeing
      // the atoms and the array by hand works on all of them.
      for (uint32_t i = 0; i < count; i++)
	JS_FreeAtom(ctx, props[i].atom);
      js_free(ctx, props);
      return ok;
    }

    // Reads the options object shared by request()/get()/post().
    bool ReadOptions(JSContext* ctx, JSValue options,
		     net::HttpClient::Request& req, bool& binary) {
      if (JS_IsUndefined(options) || JS_IsNull(options))
	return true;
      if (!JS_IsObject(options)) {
	JS_ThrowTypeError(ctx, "http: options must be an object");
	return false;
      }

      JSValue headers = JS_GetPropertyStr(ctx, options, "headers");
      bool ok = true;
      if (JS_IsObject(headers))
	ok = ReadHeaders(ctx, headers, req);
      JS_FreeValue(ctx, headers);
      if (!ok)
	return false;

      JSValue timeout = JS_GetPropertyStr(ctx, options, "timeout");
      if (JS_IsNumber(timeout)) {
	double seconds = 0.0;
	JS_ToFloat64(ctx, &seconds, timeout);
	if (seconds > 0.0)
	  req.timeout_ms = std::min((long)(seconds * 1000.0), kMaxTimeoutMs);
      }
      JS_FreeValue(ctx, timeout);

      JSValue redirect = JS_GetPropertyStr(ctx, options, "redirect");
      if (JS_IsBool(redirect))
	req.follow_redirects = JS_ToBool(ctx, redirect) != 0;
      JS_FreeValue(ctx, redirect);

      JSValue want_binary = JS_GetPropertyStr(ctx, options, "binary");
      if (JS_IsBool(want_binary))
	binary = JS_ToBool(ctx, want_binary) != 0;
      JS_FreeValue(ctx, want_binary);

      return true;
    }

    JSValue BuildResponse(JSContext* ctx, const net::HttpClient::Response& res,
			  bool binary) {
      JSValue out = JS_NewObject(ctx);
      JS_SetPropertyStr(ctx, out, "status", JS_NewInt32(ctx, (int)res.status));
      JS_SetPropertyStr(ctx, out, "ok",
			JS_NewBool(ctx, res.status >= 200 && res.status < 300));
      JS_SetPropertyStr(ctx, out, "url",
			JS_NewStringLen(ctx, res.url.data(), res.url.size()));

      JSValue headers = JS_NewObject(ctx);
      for (const auto& header : res.headers)
	JS_SetPropertyStr(ctx, headers, header.first.c_str(),
			  JS_NewStringLen(ctx, header.second.data(),
					  header.second.size()));
      JS_SetPropertyStr(ctx, out, "headers", headers);

      // A text body goes through JS_NewStringLen, which expects UTF-8;
      // 'binary: true' hands back an ArrayBuffer instead.
      if (binary)
	JS_SetPropertyStr(ctx, out, "body",
			  JS_NewArrayBufferCopy(
			    ctx, (const uint8_t*)res.body.data(),
			    res.body.size()));
      else
	JS_SetPropertyStr(ctx, out, "body",
			  JS_NewStringLen(ctx, res.body.data(),
					  res.body.size()));
      return out;
    }

    JSValue Perform(JSContext* ctx, net::HttpClient::Request& req, bool binary) {
      JsPlugin* plugin = PluginFor(ctx);
      net::HttpClient* client = plugin ? plugin->Http() : nullptr;
      if (!client)
	return ThrowError(ctx, "http: client unavailable");

      req.max_bytes = kMaxResponseBytes;

      net::HttpClient::Response res;
      std::string error;

      // Time spent blocked in libcurl is not script time; without this the
      // watchdog would kill plugins for waiting on a slow server.
      const auto started = std::chrono::steady_clock::now();
      const bool ok = client->Perform(req, res, error);
      plugin->ExtendDeadline(std::chrono::steady_clock::now() - started);

      if (!ok)
	return ThrowError(ctx, error);
      return BuildResponse(ctx, res, binary);
    }

    // http.request({ method, url, headers, body, timeout, redirect, binary })
    JSValue HttpRequest(JSContext* ctx, JSValue this_val, int argc,
			JSValue* argv) {
      (void)this_val;
      if (argc < 1 || !JS_IsObject(argv[0]))
	return JS_ThrowTypeError(ctx, "http.request: expected an options "
				 "object");

      net::HttpClient::Request req;
      bool binary = false;

      JSValue url = JS_GetPropertyStr(ctx, argv[0], "url");
      const bool have_url = JS_IsString(url) && StringArg(ctx, url, req.url);
      JS_FreeValue(ctx, url);
      if (!have_url || req.url.empty())
	return JS_ThrowTypeError(ctx, "http.request: 'url' is required");

      JSValue method = JS_GetPropertyStr(ctx, argv[0], "method");
      if (JS_IsString(method) && !StringArg(ctx, method, req.method)) {
	JS_FreeValue(ctx, method);
	return JS_EXCEPTION;
      }
      JS_FreeValue(ctx, method);
      std::transform(req.method.begin(), req.method.end(), req.method.begin(),
		     [](unsigned char c) { return std::toupper(c); });

      JSValue body = JS_GetPropertyStr(ctx, argv[0], "body");
      if (!JS_IsUndefined(body) && !JS_IsNull(body)) {
	if (!StringArg(ctx, body, req.body)) {
	  JS_FreeValue(ctx, body);
	  return JS_EXCEPTION;
	}
	req.has_body = true;
      }
      JS_FreeValue(ctx, body);

      if (!ReadOptions(ctx, argv[0], req, binary))
	return JS_EXCEPTION;
      return Perform(ctx, req, binary);
    }

    // http.get(url, options?)
    JSValue HttpGet(JSContext* ctx, JSValue this_val, int argc,
		    JSValue* argv) {
      (void)this_val;
      net::HttpClient::Request req;
      bool binary = false;
      if (argc < 1 || !JS_IsString(argv[0]) || !StringArg(ctx, argv[0], req.url))
	return JS_ThrowTypeError(ctx, "http.get: expected a url string");
      if (argc > 1 && !ReadOptions(ctx, argv[1], req, binary))
	return JS_EXCEPTION;
      return Perform(ctx, req, binary);
    }

    // http.post(url, body, options?)
    JSValue HttpPost(JSContext* ctx, JSValue this_val, int argc,
		     JSValue* argv) {
      (void)this_val;
      net::HttpClient::Request req;
      req.method = "POST";
      bool binary = false;
      if (argc < 1 || !JS_IsString(argv[0]) || !StringArg(ctx, argv[0], req.url))
	return JS_ThrowTypeError(ctx, "http.post: expected a url string");
      if (argc > 1 && !JS_IsUndefined(argv[1]) && !JS_IsNull(argv[1])) {
	if (!StringArg(ctx, argv[1], req.body))
	  return JS_EXCEPTION;
	req.has_body = true;
      }
      if (argc > 2 && !ReadOptions(ctx, argv[2], req, binary))
	return JS_EXCEPTION;
      return Perform(ctx, req, binary);
    }

  }

  void InstallHttp(JSContext* ctx) {
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue http = JS_NewObject(ctx);

    JS_SetPropertyStr(ctx, http, "request",
		      JS_NewCFunction(ctx, HttpRequest, "request", 1));
    JS_SetPropertyStr(ctx, http, "get",
		      JS_NewCFunction(ctx, HttpGet, "get", 2));
    JS_SetPropertyStr(ctx, http, "post",
		      JS_NewCFunction(ctx, HttpPost, "post", 3));

    JS_SetPropertyStr(ctx, global, "http", http);
    JS_FreeValue(ctx, global);
  }

}
