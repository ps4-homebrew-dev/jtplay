// SPDX-License-Identifier: GPL-3.0-or-later
//
// See js_source.h.
#include "browse/js_source.h"

#include <dirent.h>
#include <sys/stat.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <map>

#include "browse/js_util.h"
#include "icons.h"
#include "net/cancel.h"
#include "net/http_client.h"
#include "util/atomic_file.h"

namespace browse {
namespace {

using namespace js;

// A date field as seconds since the epoch, 0 when absent or invalid.
// Accepts whatever 'new Date(x)' does: milliseconds since the epoch, an
// ISO 8601 string or a Date object.
int64_t FieldDate(JSContext* ctx, JSValue obj, const char* key) {
  Ref v(ctx, JS_GetPropertyStr(ctx, obj, key));
  if (JS_IsException(v.get())) {
    ClearException(ctx);
    return 0;
  }
  if (JS_IsUndefined(v.get()) || JS_IsNull(v.get()))
    return 0;
  Ref global(ctx, JS_GetGlobalObject(ctx));
  Ref ctor(ctx, JS_GetPropertyStr(ctx, global.get(), "Date"));
  JSValue arg = v.get();
  Ref date(ctx, JS_CallConstructor(ctx, ctor.get(), 1, &arg));
  double ms = 0;
  if (JS_IsException(date.get()) || JS_ToFloat64(ctx, &ms, date.get()) < 0) {
    ClearException(ctx);
    return 0;
  }
  if (!std::isfinite(ms) || ms <= 0)
    return 0;
  return (int64_t)(ms / 1000.0);
}

// Converts one entry object into 'out'. Only 'name' is required; 'id'
// defaults to the uri, then to the name, so a plugin listing plain streams
// need not invent one.
bool ToEntry(JSContext* ctx, JSValue v, Entry& out, std::string& error) {
  if (!JS_IsObject(v)) {
    error = "not an object";
    return false;
  }

  out.name = FieldString(ctx, v, "name");
  if (out.name.empty()) {
    error = "has no name";
    return false;
  }
  out.type = TypeFromName(FieldString(ctx, v, "type"));
  out.description = FieldString(ctx, v, "description");
  out.image = FieldString(ctx, v, "image");
  out.uri = FieldString(ctx, v, "uri");
  out.id = FieldString(ctx, v, "id", out.uri.empty() ? out.name : out.uri);
  out.size = (int64_t)FieldNumber(ctx, v, "size", -1);
  out.date = FieldDate(ctx, v, "date");
  out.duration = (int64_t)FieldNumber(ctx, v, "duration", 0);
  return true;
}

// The mirror of ToEntry(): hands an entry back to the script, so
// resolve() can look at more than the bare id.
JSValue EntryToJs(JSContext* ctx, const Entry& e) {
  JSValue obj = JS_NewObject(ctx);
  if (JS_IsException(obj))
    return obj;

  auto set_str = [&](const char* key, const std::string& value) {
    if (!value.empty())
      JS_SetPropertyStr(ctx, obj, key, NewString(ctx, value));
  };
  set_str("id", e.id);
  JS_SetPropertyStr(ctx, obj, "type", JS_NewString(ctx, TypeName(e.type)));
  set_str("name", e.name);
  set_str("description", e.description);
  set_str("image", e.image);
  set_str("uri", e.uri);
  if (e.size >= 0)
    JS_SetPropertyStr(ctx, obj, "size", JS_NewFloat64(ctx, (double)e.size));
  if (e.date > 0)
    JS_SetPropertyStr(ctx, obj, "date", JS_NewFloat64(ctx, e.date * 1000.0));
  if (e.duration > 0)
    JS_SetPropertyStr(ctx, obj, "duration", JS_NewFloat64(ctx, (double)e.duration));
  return obj;
}

// Converts the array of entry objects returned by browse() or search()
// ('function' names the caller in messages). An entry that cannot be used
// is left out and remarked on in the log: one bad row out of a backend's
// five hundred is no reason to show none of them.
bool ToListing(const JsPlugin& plugin, JSValue v, Listing& out, std::string& error,
               const char* function) {
  JSContext* ctx = plugin.ctx;
  int64_t n = 0;
  if (!ArrayLength(ctx, v, n)) {
    error = std::string(function) + "() did not return an array";
    return false;
  }

  out.reserve(out.size() + (size_t)n);
  int skipped = 0;
  std::string first_problem;
  for (int64_t i = 0; i < n; i++) {
    Ref item(ctx, JS_GetPropertyUint32(ctx, v, (uint32_t)i));
    if (JS_IsException(item.get())) {
      error = TakeException(ctx);
      return false;
    }
    Entry e;
    std::string problem;
    if (!ToEntry(ctx, item.get(), e, problem)) {
      if (!skipped++)
        first_problem = "entry " + std::to_string(i + 1) + " " + problem;
      continue;
    }
    out.push_back(std::move(e));
  }
  if (skipped)
    plugin.Log(JsLogLevel::Warning,
               std::string(function) + "(): left out " + std::to_string(skipped) + " of " +
                 std::to_string(n) + " entries (" + first_problem + ")");
  return true;
}

// What browse() or search() returned: the array of entries (everything
// there is), or one page of them as { entries: [...], next: cursor }, the
// cursor being a string or a number that is handed back for the page
// after. No 'next' (or an empty one) is the last page.
bool ToPage(const JsPlugin& plugin, JSValue v, Listing& out, std::string& next,
            std::string& error, const char* function) {
  JSContext* ctx = plugin.ctx;
  next.clear();
  int64_t n = 0;
  if (ArrayLength(ctx, v, n) || !JS_IsObject(v))
    return ToListing(plugin, v, out, error, function);
  Ref entries(ctx, JS_GetPropertyStr(ctx, v, "entries"));
  if (JS_IsException(entries.get())) {
    error = TakeException(ctx);
    return false;
  }
  if (!ArrayLength(ctx, entries.get(), n)) {
    error = std::string(function) + "() returned neither an array nor { entries, next }";
    return false;
  }
  if (!ToListing(plugin, entries.get(), out, error, function))
    return false;
  next = FieldString(ctx, v, "next");
  return true;
}

// What resolve() returned, as something to play: the uri itself, or an
// object { uri, headers: { name: value }, subtitles: [{ uri, lang, name }] }
// (an entry object, as browse() returns them, is such an object too).
// { name: value } into a header list; a header is one line, and anything
// else is not passed on.
void ReadHeaders(JSContext* ctx, JSValue obj,
                 std::vector<std::pair<std::string, std::string>>& out) {
  if (!JS_IsObject(obj))
    return;
  const bool ok = ForEachProperty(ctx, obj, [&](const char* name, JSValue value) {
    const std::string text = ToStdString(ctx, value);
    if (*name && std::strpbrk(name, "\r\n:") == nullptr &&
        text.find_first_of("\r\n") == std::string::npos)
      out.emplace_back(name, text);
    return true;
  });
  if (!ok)
    ClearException(ctx);
}

bool ToPlayable(JSContext* ctx, JSValue v, Playable& out, std::string& error) {
  if (JS_IsString(v)) {
    out.uri = ToStdString(ctx, v);
  } else if (JS_IsObject(v)) {
    out.uri = FieldString(ctx, v, "uri");
    if (out.uri.empty())
      out.uri = FieldString(ctx, v, "url");

    Ref headers(ctx, JS_GetPropertyStr(ctx, v, "headers"));
    ReadHeaders(ctx, headers.get(), out.headers);

    Ref subtitles(ctx, JS_GetPropertyStr(ctx, v, "subtitles"));
    int64_t n = 0;
    if (ArrayLength(ctx, subtitles.get(), n)) {
      for (int64_t i = 0; i < n && i < 64; i++) {
        Ref item(ctx, JS_GetPropertyUint32(ctx, subtitles.get(), (uint32_t)i));
        Playable::Subtitle sub;
        if (JS_IsString(item.get())) {
          sub.uri = ToStdString(ctx, item.get());
        } else if (JS_IsObject(item.get())) {
          sub.uri = FieldString(ctx, item.get(), "uri");
          if (sub.uri.empty())
            sub.uri = FieldString(ctx, item.get(), "url");
          sub.language = FieldString(ctx, item.get(), "lang");
          if (sub.language.empty())
            sub.language = FieldString(ctx, item.get(), "language");
          sub.title = FieldString(ctx, item.get(), "name");
          if (sub.title.empty())
            sub.title = FieldString(ctx, item.get(), "title");
        }
        if (!sub.uri.empty())
          out.subtitles.push_back(std::move(sub));
      }
    }
  }

  if (JS_IsObject(v)) {
    Ref avoid(ctx, JS_GetPropertyStr(ctx, v, "avoidAudio"));
    int64_t n = 0;
    if (ArrayLength(ctx, avoid.get(), n))
      for (int64_t i = 0; i < n && i < 16; i++) {
        const std::string lang =
          ToStdString(ctx, Ref(ctx, JS_GetPropertyUint32(ctx, avoid.get(), (uint32_t)i)).get());
        if (!lang.empty())
          out.avoid_audio.push_back(lang);
      }
  }

  if (out.uri.empty()) {
    error = "resolve() did not return a uri";
    return false;
  }
  return true;
}

bool FileStamp(const std::string& path, int64_t& mtime, int64_t& size) {
  struct stat st;
  if (::stat(path.c_str(), &st) != 0 || !S_ISREG(st.st_mode))
    return false;
  mtime = (int64_t)st.st_mtime;
  size = (int64_t)st.st_size;
  return true;
}

} // namespace

// ---------------------------------------------------------------------------
// JsSource
// ---------------------------------------------------------------------------

JsSource::JsSource(JsPluginPtr plugin, JSValue source, std::string name, std::string detail,
                   std::string icon, std::string image, std::string root, bool can_search)
  : plugin_(std::move(plugin)), source_(source), name_(std::move(name)),
    detail_(std::move(detail)),
    // A plugin names its icon as an emoji; the UI draws its own icons.
    icon_(icon::FromEmoji(icon)), image_(std::move(image)), root_(std::move(root)),
    can_search_(can_search) {}

JsSource::~JsSource() {
  // Not freed here: that would mean waiting for the plugin's lock, which a
  // slow browse elsewhere may hold, and this may be the main thread.
  plugin_->Release(source_);
}

std::string JsSource::Key() const {
  return "js:" + plugin_->ProviderName() + "/" + name_;
}

// The first page only, to callers that want no more (a folder's preview,
// a favorite folder's listing on Home).
bool JsSource::Browse(const std::string& id, Listing& out, std::string& error) {
  std::string next;
  return BrowsePage(id, std::string(), out, next, error);
}

bool JsSource::BrowsePage(const std::string& id, const std::string& cursor, Listing& out,
                          std::string& next, std::string& error) {
  std::lock_guard<std::mutex> lock(plugin_->mutex);
  JSContext* ctx = plugin_->ctx;

  Ref arg(ctx, NewString(ctx, id));
  Ref more(ctx, NewString(ctx, cursor));
  JSValue argv[] = {arg.get(), more.get()};
  JSValue result;
  if (!plugin_->Call(source_, "browse", cursor.empty() ? 1 : 2, argv, result, error))
    return false;
  Ref owned(ctx, result);
  return ToPage(*plugin_, result, out, next, error, "browse");
}

bool JsSource::Search(const std::string& folder_id, const std::string& query, Listing& out,
                      std::string& error) {
  std::string next;
  return SearchPage(folder_id, query, std::string(), out, next, error);
}

bool JsSource::SearchPage(const std::string& folder_id, const std::string& query,
                          const std::string& cursor, Listing& out, std::string& next,
                          std::string& error) {
  std::lock_guard<std::mutex> lock(plugin_->mutex);
  JSContext* ctx = plugin_->ctx;

  Ref q(ctx, NewString(ctx, query));
  Ref id(ctx, NewString(ctx, folder_id));
  Ref more(ctx, NewString(ctx, cursor));
  JSValue argv[] = {q.get(), id.get(), more.get()};
  JSValue result;
  if (!plugin_->Call(source_, "search", cursor.empty() ? 2 : 3, argv, result, error))
    return false;
  Ref owned(ctx, result);
  return ToPage(*plugin_, result, out, next, error, "search");
}

bool JsSource::Resolve(const Entry& entry, Playable& out, std::string& error) {
  std::lock_guard<std::mutex> lock(plugin_->mutex);
  JSContext* ctx = plugin_->ctx;

  Ref id(ctx, NewString(ctx, entry.id));
  Ref obj(ctx, EntryToJs(ctx, entry));
  JSValue argv[] = {id.get(), obj.get()};
  JSValue result;
  bool missing = false;
  if (!plugin_->Call(source_, "resolve", 2, argv, result, error, &missing))
    return false;
  // No resolve(): the uri from browse() is all there is.
  if (missing)
    return Source::Resolve(entry, out, error);
  Ref owned(ctx, result);
  return ToPlayable(ctx, result, out, error);
}

// ---------------------------------------------------------------------------
// Discovery
// ---------------------------------------------------------------------------

bool DiscoverJsSources(const JsPluginPtr& plugin, std::vector<SourcePtr>& out,
                       std::string& error) {
  if (!plugin->LoadError().empty()) {
    error = plugin->LoadError();
    return false;
  }

  std::vector<SourcePtr> found;

  std::lock_guard<std::mutex> lock(plugin->mutex);
  JSContext* ctx = plugin->ctx;

  JSValue result;
  if (!plugin->Call(plugin->provider, "discover", 0, nullptr, result, error))
    return false;
  Ref owned(ctx, result);

  int64_t n = 0;
  if (!ArrayLength(ctx, result, n)) {
    error = "discover() did not return an array";
    return false;
  }

  for (int64_t i = 0; i < n; i++) {
    Ref item(ctx, JS_GetPropertyUint32(ctx, result, (uint32_t)i));
    if (JS_IsException(item.get())) {
      error = TakeException(ctx);
      return false;
    }
    if (!JS_IsObject(item.get())) {
      error = "source " + std::to_string(i + 1) + " is not an object";
      return false;
    }

    const std::string name = FieldString(ctx, item.get(), "name");
    if (name.empty()) {
      error = "source " + std::to_string(i + 1) + " has no name";
      return false;
    }
    if (!HasFunction(ctx, item.get(), "browse")) {
      error = "source '" + name + "' has no browse() function";
      return false;
    }
    const std::string detail = FieldString(ctx, item.get(), "detail", plugin->ProviderName());
    const std::string icon = FieldString(ctx, item.get(), "icon", "\U0001F9E9");
    const std::string image = FieldString(ctx, item.get(), "image", "");
    const std::string root = FieldString(ctx, item.get(), "root", "/");
    // Looked up here, with the plugin's mutex held, so that asking
    // whether it can search later (on the main thread) never enters it.
    const bool can_search = HasFunction(ctx, item.get(), "search");

    // imageHeaders: { "<URL prefix>": { name: value } }, sent when the app
    // downloads artwork from there (see docs/plugins.md).
    Ref image_headers(ctx, JS_GetPropertyStr(ctx, item.get(), "imageHeaders"));
    if (JS_IsObject(image_headers.get())) {
      const bool ok = ForEachProperty(ctx, image_headers.get(),
                                      [&](const char* prefix, JSValue rule) {
        net::HeaderList headers;
        ReadHeaders(ctx, rule, headers);
        net::SetPrefixHeaders(prefix, std::move(headers));
        return true;
      });
      if (!ok)
        ClearException(ctx);
    }

    found.push_back(std::make_shared<JsSource>(plugin, item.release(), name, detail, icon, image,
                                               root, can_search));
  }

  out.insert(out.end(), std::make_move_iterator(found.begin()),
             std::make_move_iterator(found.end()));
  return true;
}

// ---------------------------------------------------------------------------
// JsProvider
// ---------------------------------------------------------------------------

JsProvider::JsProvider(std::vector<std::string> dirs, std::shared_ptr<const JsEnvironment> env)
  : dirs_(std::move(dirs)), env_(std::move(env)) {}

std::vector<JsPluginPtr> JsProvider::Plugins() const {
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<JsPluginPtr> out;
  out.reserve(loaded_.size());
  for (const Loaded& l : loaded_)
    out.push_back(l.plugin);
  return out;
}

void JsProvider::ReadSettingsFile(const std::string& path) {
  int64_t mtime = 0, size = 0;
  if (!FileStamp(path, mtime, size)) {
    settings_files_.erase(path);
    return;
  }
  const auto stamp = std::make_pair(mtime, size);
  const auto known = settings_files_.find(path);
  if (known != settings_files_.end() && known->second == stamp)
    return;
  settings_files_[path] = stamp;

  std::string text;
  if (!util::ReadFile(path, text, 1u << 20))
    return;
  size_t at = 0, applied = 0;
  while (at < text.size()) {
    size_t end = text.find('\n', at);
    if (end == std::string::npos)
      end = text.size();
    std::string line = text.substr(at, end - at);
    at = end + 1;
    while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t'))
      line.pop_back();
    const size_t begin = line.find_first_not_of(" \t");
    if (begin == std::string::npos || line[begin] == '#' || line[begin] == ';')
      continue;
    const size_t eq = line.find('=', begin);
    if (eq == std::string::npos)
      continue;
    std::string key = line.substr(begin, eq - begin);
    while (!key.empty() && (key.back() == ' ' || key.back() == '\t'))
      key.pop_back();
    // As the app's own settings.ini has them, too.
    if (key.compare(0, 7, "plugin.") == 0)
      key.erase(0, 7);
    const size_t value_at = line.find_first_not_of(" \t", eq + 1);
    const std::string value = value_at == std::string::npos ? std::string()
                                                            : line.substr(value_at);
    if (key.find('.') == std::string::npos || key.front() == '.' || key.back() == '.')
      continue;
    if (env_->settings->Get(key.substr(0, key.find('.')), key.substr(key.find('.') + 1)) !=
        value)
      env_->settings->SetNamed(key, value);
    applied++;
  }
  std::fprintf(stderr, "plugins: %zu setting%s from %s\n", applied, applied == 1 ? "" : "s",
               path.c_str());
}

// Brings loaded_ up to date with the *.js files of the plugin directories.
void JsProvider::Reload() {
  for (const std::string& dir : dirs_)
    ReadSettingsFile(dir + "/settings.ini");

  // File name -> path; a later directory wins. A missing or unreadable
  // directory is not an error; it just adds nothing.
  std::map<std::string, std::string> scripts;
  for (const std::string& dir : dirs_) {
    DIR* d = ::opendir(dir.c_str());
    if (!d)
      continue;
    while (struct dirent* de = ::readdir(d)) {
      const std::string name = de->d_name;
      if (name.size() > 3 && name[0] != '.' && name.compare(name.size() - 3, 3, ".js") == 0)
        scripts[name] = dir + "/" + name;
    }
    ::closedir(d);
  }

  std::vector<Loaded> before;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    before = loaded_;
  }

  std::vector<Loaded> now;
  for (const auto& script : scripts) { // by file name
    Loaded l;
    l.path = script.second;
    if (!FileStamp(l.path, l.mtime, l.size))
      continue;
    for (const Loaded& old : before) {
      if (old.path == l.path && old.mtime == l.mtime && old.size == l.size) {
        l.plugin = old.plugin;
        break;
      }
    }
    // Evaluating a script takes a moment; not under the lock, which the
    // main thread takes to look at the plugins.
    if (!l.plugin)
      l.plugin = std::make_shared<JsPlugin>(l.path, env_);
    now.push_back(std::move(l));
  }

  std::sort(now.begin(), now.end(), [](const Loaded& a, const Loaded& b) {
    return a.plugin->ProviderName() < b.plugin->ProviderName();
  });
  std::lock_guard<std::mutex> lock(mutex_);
  loaded_ = std::move(now);
}

bool JsProvider::Discover(std::vector<SourcePtr>& out, std::string& error) {
  Reload();

  bool ok = true;
  for (const JsPluginPtr& plugin : Plugins()) {
    std::string problem;
    if (DiscoverJsSources(plugin, out, problem))
      continue;
    ok = false;
    if (!error.empty())
      error += "; ";
    error += plugin->ProviderName() + ": " + problem;
  }
  return ok;
}

} // namespace browse
