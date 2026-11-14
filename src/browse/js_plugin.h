// SPDX-License-Identifier: GPL-3.0-or-later
//
// One JavaScript plugin: a script file evaluated in a QuickJS runtime of its
// own, and everything the host puts at its disposal there. The script's side
// of it is described in docs/plugins.md; in short it gets
//
//   console    log / info / debug / warn / error
//   http       get / post / request / json / all      (js_http.cpp)
//   storage    get / set / remove / keys: a small JSON file per plugin
//   settings   get: what the user chose on the Settings page
//   util       query, uuid, memo, text, decodeEntities, resolveUrl, ...
//   xml        parse and a few accessors
//   atob, btoa
//   jtplay     { api, version, platform }
//
// The natives are in js_plugin.cpp and js_http.cpp; what can be written in
// JavaScript is, in the prelude (js_prelude.h), which is evaluated in every
// runtime before the plugin's script.
#ifndef BROWSE_JS_PLUGIN_H
#define BROWSE_JS_PLUGIN_H

#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <quickjs.h>

#include "browse/plugin_settings.h"
#include "net/http_client.h"

namespace browse {

// The version of the plugin interface, as 'jtplay.api'. Goes up when
// something is added; nothing a script relied on has been taken away.
//   1  http, console; discover / browse / search / resolve
//   2  http.all and http.json, storage, settings, util, xml, atob / btoa;
//      resolve() may return headers and subtitles; functions may be async
inline constexpr int kJsApiVersion = 2;

enum class JsLogLevel { Info, Warning, Error };

// What the app provides to every plugin. Shared, and not changed once
// plugins have been made with it.
struct JsEnvironment {
  // Where storage is kept, one file per plugin; "" = forgotten at exit.
  std::string data_dir;
  std::string app_version;
  std::string platform; // "ps4", "linux", ...
  std::shared_ptr<PluginSettings> settings = std::make_shared<PluginSettings>();
  // For tests: a request whose URL begins with 'first' goes to 'second'
  // plus the rest instead, so a plugin can be run against a stand-in for
  // the service it talks to. The app sets none.
  std::vector<std::pair<std::string, std::string>> url_rewrites;
  // console.*, and the host's own remarks about a plugin. Called from
  // whatever thread the plugin runs on. Unset: standard error.
  std::function<void(JsLogLevel, const std::string& plugin, const std::string& text)> log;
};

// One loaded plugin script: owns the QuickJS runtime and the provider
// object the script returned. Shared by the provider and every source it
// discovers, so an in-flight browse keeps the runtime alive across a
// rescan.
//
// A runtime may only be touched by one thread at a time, hence 'mutex':
// whoever uses 'ctx' or 'provider', or calls a member marked "runtime",
// holds it. The script is evaluated on one thread and called from others,
// so every entry point also refreshes QuickJS' notion of where the stack
// starts (Call() does).
class JsPlugin {
public:
  JsPlugin(const std::string& path, std::shared_ptr<const JsEnvironment> env);
  ~JsPlugin();

  JsPlugin(const JsPlugin&) = delete;
  JsPlugin& operator=(const JsPlugin&) = delete;

  // "" once the script loaded and returned a well-formed provider object.
  const std::string& LoadError() const { return load_error_; }
  // The provider's name (falls back to the file name).
  const std::string& ProviderName() const { return name_; }
  // The file name without ".js": names the plugin's storage and settings.
  const std::string& Id() const { return id_; }
  const std::string& Path() const { return path_; }
  // The settings the script declared. Fixed once loaded.
  const std::vector<PluginSetting>& Settings() const { return settings_; }
  const JsEnvironment& Environment() const { return *env_; }

  // Any thread: the value of a declared setting (its fallback until the
  // user has picked one; "" for one that was never declared).
  std::string SettingValue(const std::string& key) const;

  // Runtime. Calls object.function(args...) with 'this' set to 'object',
  // under a watchdog and the calling thread's cancel token. When the script
  // returns a promise (an async function), it is waited for. On success
  // 'result' holds a reference the caller frees; otherwise 'error' says
  // what went wrong, and is net::kCancelledError when given up on.
  //
  // 'missing' (if given) is set when the object has no such function, which
  // then is not an error: 'result' is undefined and the call returns true.
  bool Call(JSValue object, const char* function, int argc, JSValue* argv, JSValue& result,
            std::string& error, bool* missing = nullptr);

  // Runtime. Pushes the watchdog's deadline back by 'by'. Blocking native
  // calls (an HTTP request) are not script time, so they must not count.
  void ExtendDeadline(std::chrono::steady_clock::duration by);

  // Runtime. The plugin's HTTP client; null if libcurl could not be set up.
  net::HttpClient* Http() const { return http_.get(); }

  void Log(JsLogLevel level, const std::string& text) const;

  // Any thread, no lock needed: lets go of a reference into the runtime
  // (a source object, when its JsSource goes). It is only noted here and
  // freed the next time the runtime is entered, so that dropping a source
  // on the main thread never waits for a script that is busy elsewhere.
  void Release(JSValue value);

  std::mutex mutex;         // serializes all runtime access
  JSRuntime* rt = nullptr;
  JSContext* ctx = nullptr; // null if the runtime could not be created
  JSValue provider;         // provider object; JS_UNDEFINED if none

  // Natives (js_plugin.cpp) reach these through JS_GetContextOpaque().
  std::string StoragePath() const;

private:
  static int InterruptHandler(JSRuntime* rt, void* opaque);
  bool Evaluate(const std::string& code, const char* name, JSValue& result, std::string& error);
  // Waits for the promise 'value' and replaces it with what it resolved to.
  bool Settle(JSValue& value, const char* function, std::string& error);
  // The pending exception as an error message, told apart from a watchdog
  // or a cancel.
  std::string CallError();
  void ReadSettings();
  void FreeReleased(); // runtime

  std::shared_ptr<const JsEnvironment> env_;
  std::unique_ptr<net::HttpClient> http_;
  std::chrono::steady_clock::time_point deadline_;
  bool armed_ = false;
  bool timed_out_ = false;
  JSValue settle_;          // the prelude's promise helper
  std::mutex released_mutex_;
  std::vector<JSValue> released_; // see Release()
  std::string path_;
  std::string id_;
  std::string name_;
  std::string load_error_;
  std::vector<PluginSetting> settings_;
};

using JsPluginPtr = std::shared_ptr<JsPlugin>;

// Installs 'http' on the context's global object (js_http.cpp). The client
// comes from the JsPlugin behind JS_GetContextOpaque().
void InstallHttp(JSContext* ctx);

} // namespace browse

#endif
