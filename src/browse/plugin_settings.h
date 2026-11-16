// SPDX-License-Identifier: GPL-3.0-or-later
//
// Settings a plugin asks the user for: declared by the script (see
// PluginSetting), shown on the app's Settings page and in the options menu
// of what is the plugin's, read back by the script
// with settings.get(). The values live here, between the two: the app
// changes them on the main thread, plugins read them on worker threads.
#ifndef BROWSE_PLUGIN_SETTINGS_H
#define BROWSE_PLUGIN_SETTINGS_H

#include <atomic>
#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace browse {

// One setting as a plugin declares it: a choice among a few options, or a
// line of text (a sign-in token, a server address).
struct PluginSetting {
  std::string key;      // what the script asks for
  std::string label;    // "Country"
  std::string about;    // a sentence for the panel beside the list
  // (value, label) pairs, in the order they are stepped through. Empty for
  // a text setting.
  std::vector<std::pair<std::string, std::string>> options;
  std::string fallback; // the value until the user picks one
  bool text = false;    // typed in rather than picked
  bool secret = false;  // text that is never shown, only whether it is set
};

// Thread safe. Keys are "<plugin id>.<setting key>", which is also how the
// app writes them to its settings file.
class PluginSettings {
public:
  static std::string Name(const std::string& plugin, const std::string& key) {
    return plugin + "." + key;
  }

  // The stored value, or 'fallback' when there is none.
  std::string Get(const std::string& plugin, const std::string& key,
                  const std::string& fallback = std::string()) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = values_.find(Name(plugin, key));
    return it == values_.end() ? fallback : it->second;
  }

  void Set(const std::string& plugin, const std::string& key, const std::string& value) {
    SetNamed(Name(plugin, key), value);
  }

  void SetNamed(const std::string& name, const std::string& value) {
    std::lock_guard<std::mutex> lock(mutex_);
    values_[name] = value;
    revision_++;
  }

  // Everything stored, for writing out.
  std::map<std::string, std::string> All() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return values_;
  }

  // Goes up with every change: what a plugin derived from its settings (a
  // cached listing) is stale once this has moved.
  uint64_t Revision() const { return revision_.load(); }

private:
  mutable std::mutex mutex_;
  std::map<std::string, std::string> values_;
  std::atomic<uint64_t> revision_{0};
};

} // namespace browse

#endif
