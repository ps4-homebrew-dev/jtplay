// SPDX-License-Identifier: GPL-3.0-or-later
//
// browse::Source / browse::Provider backed by JavaScript plugins (see
// js_plugin.h for the scripts' side of it, docs/plugins.md for authors).
#ifndef BROWSE_JS_SOURCE_H
#define BROWSE_JS_SOURCE_H

#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <quickjs.h>

#include "browse/js_plugin.h"
#include "browse/source.h"

namespace browse {

// One source object out of a plugin's discover().
class JsSource : public Source {
public:
  // Takes ownership of 'source' (a reference to the source object).
  JsSource(JsPluginPtr plugin, JSValue source, std::string name, std::string detail,
           std::string icon, std::string image, std::string root, bool can_search);
  ~JsSource() override;

  const std::string& Name() const override { return name_; }
  const std::string& Detail() const override { return detail_; }
  const char* Icon() const override { return icon_.c_str(); }
  const std::string& Image() const override { return image_; }
  std::string RootId() const override { return root_; }
  std::string Key() const override;

  bool Browse(const std::string& id, Listing& out, std::string& error) override;

  // A source can search when its script has a search(query, folderId)
  // function, which returns entries like browse() does. Whether there is
  // one is looked up once, when the source is made, so this never has to
  // enter the script.
  bool CanSearch(const std::string&) const override { return can_search_; }
  bool Search(const std::string& folder_id, const std::string& query, Listing& out,
              std::string& error) override;

  // Calls the script's resolve(id, entry) when it has one, so a plugin
  // serving signed URIs can hand out a fresh one per playback, with the
  // headers and subtitles that go with it. Without a resolve function this
  // falls back to Source::Resolve().
  bool Resolve(const Entry& entry, Playable& out, std::string& error) override;

private:
  JsPluginPtr plugin_;
  JSValue source_;
  std::string name_;
  std::string detail_;
  std::string icon_;
  std::string image_;
  std::string root_;
  bool can_search_;
};

// Every plugin script of the plugin directories, as one provider.
//
// Discover() first brings the set of loaded scripts up to date with what is
// on disk: new files are loaded, changed ones loaded afresh, deleted ones
// let go. A plugin can so be added or edited while the app runs, and shows
// up at the next search of the Plugins tab. An unchanged script keeps its
// runtime, and with it whatever it has cached.
//
// A script that fails to load does not hold up the others: what it said is
// part of 'error', and Discover() still returns the sources of the rest.
//
// A file 'settings.ini' in a plugin directory gives plugins' settings, one
// per line as "<plugin>.<key>=<value>" (a sign-in token, which nobody wants
// to type with a controller, put there over FTP). It is read with the
// scripts, when it is new or has changed, and its values replace what the
// settings were.
class JsProvider : public Provider {
public:
  // 'dirs' are searched in order; a file name found in a later directory
  // replaces the one of an earlier (a user's copy overrides the bundled).
  JsProvider(std::vector<std::string> dirs, std::shared_ptr<const JsEnvironment> env);

  // Its errors name the plugin they are about; "" says not to prefix them.
  const char* Name() const override { return ""; }
  bool Discover(std::vector<SourcePtr>& out, std::string& error) override;

  // Any thread: the plugins as of the last Discover(), by name.
  std::vector<JsPluginPtr> Plugins() const;

private:
  struct Loaded {
    std::string path;
    int64_t mtime = 0;
    int64_t size = 0;
    JsPluginPtr plugin;
  };
  void Reload();
  void ReadSettingsFile(const std::string& path);

  std::vector<std::string> dirs_;
  std::shared_ptr<const JsEnvironment> env_;
  mutable std::mutex mutex_; // guards loaded_
  std::vector<Loaded> loaded_;
  // Settings files as last read: path -> (mtime, size). Discovery lane only.
  std::map<std::string, std::pair<int64_t, int64_t>> settings_files_;
};

// The sources of one plugin, as its discover() describes them. False with
// the reason in 'error' when the script failed to load or discover() did.
bool DiscoverJsSources(const JsPluginPtr& plugin, std::vector<SourcePtr>& out,
                       std::string& error);

} // namespace browse

#endif
