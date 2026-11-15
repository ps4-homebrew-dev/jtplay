// SPDX-License-Identifier: GPL-3.0-or-later
//
// Runs a plugin script without the app around it: no window, no player.
// For writing plugins (edit, run, read the answer) and for the tests.
//
//   jtplay --plugin FILE                       its sources
//   jtplay --plugin FILE --browse ID           a folder ("" or no ID: root)
//   jtplay --plugin FILE --search QUERY        what search() finds
//   jtplay --plugin FILE --resolve ID          what would be played
//
// Answers are JSON on standard output, console.log() and errors go to
// standard error, and the exit status says whether it worked. See
// docs/plugins.md.
#include "plugin_cli.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "browse/js_source.h"

namespace {

void Usage(const char* argv0) {
  std::fprintf(stderr,
    "usage: %s --plugin FILE [options] [action]\n"
    "\n"
    "actions (default: list the plugin's sources and settings)\n"
    "  --browse [ID]      list a folder; without ID, the source's root\n"
    "  --search QUERY     ask the source's search()\n"
    "  --resolve ID       what resolve() makes of the item ID\n"
    "\n"
    "options\n"
    "  --source N|NAME    which of the plugin's sources (default: the first)\n"
    "  --folder ID        --search: the folder searched from\n"
    "  --uri URI          --resolve: the item's uri, as browse() listed it\n"
    "  --set KEY=VALUE    one of the plugin's settings, for this run\n"
    "  --data DIR         where the plugin's storage is kept (default: none,\n"
    "                     so nothing outlives the run)\n"
    "  --rewrite FROM=TO  send requests for URLs beginning with FROM to TO\n"
    "                     instead (a stand-in server, for tests); repeatable\n",
    argv0);
}

std::string Quote(const std::string& s) {
  std::string out = "\"";
  for (unsigned char c : s) {
    switch (c) {
    case '"':  out += "\\\""; break;
    case '\\': out += "\\\\"; break;
    case '\n': out += "\\n"; break;
    case '\r': out += "\\r"; break;
    case '\t': out += "\\t"; break;
    default:
      if (c < 0x20) {
        char buf[8];
        std::snprintf(buf, sizeof(buf), "\\u%04x", c);
        out += buf;
      } else {
        out += (char)c;
      }
    }
  }
  return out + "\"";
}

// One "key": value member at a time; says where the commas go.
class Object {
public:
  explicit Object(std::string indent) : indent_(std::move(indent)) {}
  void String(const char* key, const std::string& value, bool always = false) {
    if (always || !value.empty())
      Member(key, Quote(value));
  }
  void Number(const char* key, long long value) { Member(key, std::to_string(value)); }
  void Bool(const char* key, bool value) { Member(key, value ? "true" : "false"); }
  void Member(const char* key, const std::string& json) {
    text_ += (text_.empty() ? "" : ",\n") + indent_ + "  " + Quote(key) + ": " + json;
  }
  std::string Text() const { return "{\n" + text_ + "\n" + indent_ + "}"; }

private:
  std::string indent_;
  std::string text_;
};

std::string Array(const std::vector<std::string>& items, const std::string& indent) {
  if (items.empty())
    return "[]";
  std::string out = "[\n";
  for (size_t i = 0; i < items.size(); i++)
    out += indent + "  " + items[i] + (i + 1 < items.size() ? ",\n" : "\n");
  return out + indent + "]";
}

std::string EntryJson(const browse::Entry& e) {
  Object o("  ");
  o.String("id", e.id, true);
  o.String("type", browse::TypeName(e.type), true);
  o.String("name", e.name, true);
  o.String("description", e.description);
  o.String("image", e.image);
  o.String("uri", e.uri);
  if (e.size >= 0)
    o.Number("size", e.size);
  if (e.date > 0)
    o.Number("date", e.date);
  if (e.duration > 0)
    o.Number("duration", e.duration);
  return o.Text();
}

void PrintListing(const browse::Listing& listing) {
  std::vector<std::string> items;
  for (const browse::Entry& e : listing)
    items.push_back(EntryJson(e));
  std::printf("%s\n", Array(items, "").c_str());
}

int Fail(const std::string& what) {
  std::fprintf(stderr, "error: %s\n", what.c_str());
  return 1;
}

} // namespace

int PluginCli(int argc, char* argv[]) {
  std::string file, source_pick, id, query, folder, uri, data_dir;
  std::vector<std::pair<std::string, std::string>> set, rewrites;
  enum class Action { Sources, Browse, Search, Resolve } action = Action::Sources;

  for (int i = 1; i < argc; i++) {
    const std::string arg = argv[i];
    auto value = [&]() -> std::string {
      if (i + 1 >= argc) {
        std::fprintf(stderr, "%s: missing argument for %s\n", argv[0], arg.c_str());
        std::exit(2);
      }
      return argv[++i];
    };
    if (arg == "--plugin") {
      file = value();
    } else if (arg == "--source") {
      source_pick = value();
    } else if (arg == "--browse") {
      action = Action::Browse;
      // The id is optional, and may well begin with a dash.
      if (i + 1 < argc && std::strncmp(argv[i + 1], "--", 2) != 0)
        id = argv[++i];
      else
        id = "\x01"; // the root
    } else if (arg == "--search") {
      action = Action::Search;
      query = value();
    } else if (arg == "--resolve") {
      action = Action::Resolve;
      id = value();
    } else if (arg == "--folder") {
      folder = value();
    } else if (arg == "--uri") {
      uri = value();
    } else if (arg == "--data") {
      data_dir = value();
    } else if (arg == "--rewrite") {
      const std::string pair = value();
      // (The first '=' after the scheme: URLs have their own further on.)
      const size_t eq = pair.find('=', pair.find("://") == std::string::npos ? 0 : pair.find("://"));
      if (eq == std::string::npos) {
        std::fprintf(stderr, "%s: --rewrite wants FROM=TO\n", argv[0]);
        return 2;
      }
      rewrites.emplace_back(pair.substr(0, eq), pair.substr(eq + 1));
    } else if (arg == "--set") {
      const std::string pair = value();
      const size_t eq = pair.find('=');
      if (eq == std::string::npos) {
        std::fprintf(stderr, "%s: --set wants KEY=VALUE\n", argv[0]);
        return 2;
      }
      set.emplace_back(pair.substr(0, eq), pair.substr(eq + 1));
    } else if (arg == "-h" || arg == "--help") {
      Usage(argv[0]);
      return 0;
    } else {
      std::fprintf(stderr, "%s: unknown option '%s'\n", argv[0], arg.c_str());
      Usage(argv[0]);
      return 2;
    }
  }
  if (file.empty()) {
    Usage(argv[0]);
    return 2;
  }

  auto env = std::make_shared<browse::JsEnvironment>();
  env->data_dir = data_dir;
#ifdef JTPLAY_VERSION
  env->app_version = JTPLAY_VERSION;
#else
  env->app_version = "dev";
#endif
  env->platform = "cli";
  env->url_rewrites = rewrites;

  const auto plugin = std::make_shared<browse::JsPlugin>(file, env);
  if (!plugin->LoadError().empty())
    return Fail(plugin->LoadError());
  for (const auto& kv : set)
    env->settings->Set(plugin->Id(), kv.first, kv.second);

  std::vector<browse::SourcePtr> sources;
  std::string error;
  if (!browse::DiscoverJsSources(plugin, sources, error))
    return Fail(error);

  if (action == Action::Sources) {
    Object out("");
    out.String("name", plugin->ProviderName(), true);
    out.String("id", plugin->Id(), true);
    std::vector<std::string> list;
    for (const browse::SourcePtr& s : sources) {
      Object o("    ");
      o.String("name", s->Name(), true);
      o.String("detail", s->Detail());
      o.String("key", s->Key(), true);
      o.String("root", s->RootId(), true);
      o.Bool("search", s->CanSearch(s->RootId()));
      list.push_back(o.Text());
    }
    out.Member("sources", Array(list, "  "));
    std::vector<std::string> settings;
    for (const browse::PluginSetting& s : plugin->Settings()) {
      Object o("    ");
      o.String("key", s.key, true);
      o.String("label", s.label, true);
      const std::string value = plugin->SettingValue(s.key);
      if (s.text) {
        o.String("type", "text", true);
        // A token on standard output would end up in a terminal's
        // scrollback, or a bug report.
        o.String("value", s.secret && !value.empty() ? "(set)" : value, true);
      } else {
        o.String("value", value, true);
        std::vector<std::string> options;
        for (const auto& option : s.options)
          options.push_back(Quote(option.first));
        o.Member("options", Array(options, "      "));
      }
      settings.push_back(o.Text());
    }
    out.Member("settings", Array(settings, "  "));
    std::printf("%s\n", out.Text().c_str());
    return 0;
  }

  if (sources.empty())
    return Fail("the plugin has no sources");
  browse::SourcePtr source = sources.front();
  if (!source_pick.empty()) {
    source.reset();
    char* end = nullptr;
    const long index = std::strtol(source_pick.c_str(), &end, 10);
    if (*end == '\0' && index >= 1 && index <= (long)sources.size())
      source = sources[(size_t)index - 1];
    for (const browse::SourcePtr& s : sources)
      if (!source && s->Name() == source_pick)
        source = s;
    if (!source)
      return Fail("no source '" + source_pick + "'");
  }

  switch (action) {
  case Action::Browse: {
    browse::Listing listing;
    if (!source->Browse(id == "\x01" ? source->RootId() : id, listing, error))
      return Fail(error);
    PrintListing(listing);
    return 0;
  }
  case Action::Search: {
    if (!source->CanSearch(folder))
      return Fail("this source has no search()");
    browse::Listing listing;
    if (!source->Search(folder.empty() ? source->RootId() : folder, query, listing, error))
      return Fail(error);
    PrintListing(listing);
    return 0;
  }
  case Action::Resolve: {
    browse::Entry entry;
    entry.id = id;
    entry.uri = uri;
    entry.name = id;
    browse::Playable playable;
    if (!source->Resolve(entry, playable, error))
      return Fail(error);
    Object out("");
    out.String("uri", playable.uri, true);
    if (!playable.headers.empty()) {
      Object headers("  ");
      for (const auto& h : playable.headers)
        headers.String(h.first.c_str(), h.second, true);
      out.Member("headers", headers.Text());
    }
    if (!playable.avoid_audio.empty()) {
      std::vector<std::string> langs;
      for (const std::string& lang : playable.avoid_audio)
        langs.push_back(Quote(lang));
      out.Member("avoidAudio", Array(langs, "  "));
    }
    if (!playable.subtitles.empty()) {
      std::vector<std::string> subs;
      for (const auto& s : playable.subtitles) {
        Object o("    ");
        o.String("uri", s.uri, true);
        o.String("lang", s.language);
        o.String("name", s.title);
        subs.push_back(o.Text());
      }
      out.Member("subtitles", Array(subs, "  "));
    }
    std::printf("%s\n", out.Text().c_str());
    return 0;
  }
  default:
    return 0;
  }
}

#ifdef JTPLAY_PLUGIN_TOOL
int main(int argc, char* argv[]) {
  return PluginCli(argc, argv);
}
#endif
