// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <chrono>
#include <memory>
#include <mutex>

#include <quickjs.h>

#include "browse/source.h"
#include "net/http_client.h"

namespace browse {

  // One loaded plugin script: owns the QuickJS runtime and the provider
  // object the script returned. Shared by the provider and every source it
  // discovers, so an in-flight browse keeps the runtime alive across a
  // rescan.
  //
  // A runtime may only be touched by one thread at a time, hence 'mutex'.
  // The script is evaluated on the main thread but called from the worker
  // thread, so every entry point also refreshes QuickJS' notion of where
  // the stack starts (see CallScope in js_source.cpp).
  class JsPlugin {
  public:
    explicit JsPlugin(const std::string& path);
    ~JsPlugin();

    JsPlugin(const JsPlugin&) = delete;
    JsPlugin& operator=(const JsPlugin&) = delete;

    // "" once the script loaded and returned a well-formed provider object.
    const std::string& LoadError() const { return load_error_; }
    const std::string& ProviderName() const { return name_; }

    // Watchdog around a single plugin call: a script that does not return
    // within the deadline is interrupted and reports an error instead of
    // wedging the worker thread forever. Callers hold 'mutex'.
    void ArmWatchdog(int timeout_ms);
    void DisarmWatchdog();

    // Pushes the deadline back by 'by'. Blocking C calls (an HTTP request)
    // are not script time, so they must not count against the watchdog.
    void ExtendDeadline(std::chrono::steady_clock::duration by);

    // The plugin's HTTP client; null if libcurl could not be initialized.
    net::HttpClient* Http() const { return http_.get(); }

    std::mutex mutex;         // serializes all runtime access
    JSRuntime* rt = nullptr;
    JSContext* ctx = nullptr; // null if the runtime could not be created
    JSValue provider;         // provider object; JS_UNDEFINED if none

  private:
    static int InterruptHandler(JSRuntime* rt, void* opaque);

    std::unique_ptr<net::HttpClient> http_;
    std::chrono::steady_clock::time_point deadline_;
    bool armed_ = false;
    std::string name_;        // provider name (falls back to the file name)
    std::string load_error_;
  };

  using JsPluginPtr = std::shared_ptr<JsPlugin>;

  class JsSource : public Source {
  public:
    // Takes ownership of 'source' (a reference to the source object).
    JsSource(JsPluginPtr plugin, JSValue source, std::string name,
	     std::string detail, std::string icon, std::string root);
    ~JsSource() override;

    const std::string& Name() const override { return name_; }
    const std::string& Detail() const override { return detail_; }
    const char* Icon() const override { return icon_.c_str(); }
    std::string RootId() const override { return root_; }

    bool Browse(const std::string& id, Listing& out,
		std::string& error) override;

    // Calls the script's resolve(id, entry) when it has one, so a plugin
    // serving signed URIs can hand out a fresh one per playback. Without
    // a resolve function this falls back to Source::Resolve().
    bool Resolve(const Entry& entry, std::string& uri,
		 std::string& error) override;

  private:
    JsPluginPtr plugin_;
    JSValue source_;
    std::string name_;
    std::string detail_;
    std::string icon_;
    std::string root_;
  };

  // One plugin script. A script that failed to load still yields a provider
  // so the failure surfaces in the sources-view status line on every scan.
  class JsProvider : public Provider {
  public:
    explicit JsProvider(const std::string& path);

    const char* Name() const override { return plugin_->ProviderName().c_str(); }
    bool Discover(std::vector<SourcePtr>& out, std::string& error) override;

  private:
    JsPluginPtr plugin_;
  };

  // Appends one JsProvider per *.js file in 'dir' (sorted by file name).
  // A missing or unreadable directory is not an error; it just adds nothing.
  void LoadJsProviders(const std::string& dir,
		       std::vector<std::unique_ptr<Provider>>& out);

}
