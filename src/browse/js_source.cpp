// SPDX-License-Identifier: GPL-3.0-or-later

#include <dirent.h>

#include <algorithm>
#include <cstdint>
#include <fstream>
#include <sstream>

#include <RmlUi/Core/Log.h>

#include "browse/js_http.h"
#include "browse/js_source.h"

namespace browse {
  namespace {

    // A plugin has no I/O bindings, so nothing it does should take long;
    // the deadline only exists to survive a runaway loop.
    constexpr int kCallTimeoutMs = 10000;
    constexpr size_t kMemoryLimitBytes = 32u << 20;

    std::string BaseName(const std::string& path) {
      const size_t slash = path.rfind('/');
      return slash == std::string::npos ? path : path.substr(slash + 1);
    }

    // Refreshes the stack base (the script is loaded on the main thread but
    // called from the worker thread) and arms the watchdog for one call.
    // Constructed with 'plugin->mutex' held.
    class CallScope {
    public:
      explicit CallScope(JsPlugin& plugin) : plugin_(plugin) {
	JS_UpdateStackTop(plugin_.rt);
	plugin_.ArmWatchdog(kCallTimeoutMs);
      }
      ~CallScope() { plugin_.DisarmWatchdog(); }

      CallScope(const CallScope&) = delete;
      CallScope& operator=(const CallScope&) = delete;

    private:
      JsPlugin& plugin_;
    };

    // Frees 'v' on scope exit; error paths would otherwise leak references.
    class Ref {
    public:
      Ref(JSContext* ctx, JSValue v) : ctx_(ctx), v_(v) {}
      ~Ref() { JS_FreeValue(ctx_, v_); }

      Ref(const Ref&) = delete;
      Ref& operator=(const Ref&) = delete;

      JSValue get() const { return v_; }

      // Hands the reference to the caller; no longer freed here.
      JSValue release() {
	JSValue v = v_;
	v_ = JS_UNDEFINED;
	return v;
      }

    private:
      JSContext* ctx_;
      JSValue v_;
    };

    std::string ToStdString(JSContext* ctx, JSValue v) {
      size_t len = 0;
      const char* s = JS_ToCStringLen(ctx, &len, v);
      if (!s) {
	// Conversion threw (a Symbol, or a throwing toString).
	JS_FreeValue(ctx, JS_GetException(ctx));
	return {};
      }
      std::string out(s, len);
      JS_FreeCString(ctx, s);
      return out;
    }

    // Takes the pending exception and renders it as one line. Errors carry
    // the script location in their stack, so the first stack frame is worth
    // keeping: "SyntaxError: unexpected token (sr.js:12)".
    std::string TakeException(JSContext* ctx) {
      Ref e(ctx, JS_GetException(ctx));
      std::string msg = ToStdString(ctx, e.get());
      if (msg.empty())
	msg = "(unknown error)";

      if (JS_IsObject(e.get())) {
	Ref stack(ctx, JS_GetPropertyStr(ctx, e.get(), "stack"));
	if (JS_IsString(stack.get())) {
	  std::string frame = ToStdString(ctx, stack.get());
	  frame = frame.substr(0, frame.find('\n'));
	  const size_t begin = frame.find_first_not_of(" \t");
	  if (begin != std::string::npos)
	    msg += " (" + frame.substr(begin) + ")";
	}
      }
      return msg;
    }

    // True for anything that looks like a promise. Plugins run without an
    // event loop, so an async function is a mistake worth naming.
    bool IsThenable(JSContext* ctx, JSValue v) {
      if (!JS_IsObject(v))
	return false;
      Ref then(ctx, JS_GetPropertyStr(ctx, v, "then"));
      return JS_IsFunction(ctx, then.get());
    }

    // Field readers; all tolerate a missing field ('obj' is the object).

    std::string FieldString(JSContext* ctx, JSValue obj, const char* key,
			    const std::string& fallback = {}) {
      Ref v(ctx, JS_GetPropertyStr(ctx, obj, key));
      if (JS_IsException(v.get())) {
	JS_FreeValue(ctx, JS_GetException(ctx));
	return fallback;
      }
      // Numbers are convenient for ids; anything else (objects, booleans,
      // null, undefined) is treated as absent rather than stringified.
      if (!JS_IsString(v.get()) && !JS_IsNumber(v.get()))
	return fallback;
      return ToStdString(ctx, v.get());
    }

    // Length of an array (or any array-like). JS_IsArray() has an
    // incompatible signature across QuickJS forks, so duck-type instead.
    bool ArrayLength(JSContext* ctx, JSValue v, int64_t& out) {
      if (!JS_IsObject(v))
	return false;
      Ref len(ctx, JS_GetPropertyStr(ctx, v, "length"));
      if (!JS_IsNumber(len.get()))
	return false;
      if (JS_ToInt64(ctx, &out, len.get()) < 0) {
	JS_FreeValue(ctx, JS_GetException(ctx));
	return false;
      }
      return out >= 0;
    }

    Entry::Type TypeFromString(const std::string& s) {
      if (s == "folder") return Entry::Type::Folder;
      if (s == "audio")  return Entry::Type::Audio;
      if (s == "video")  return Entry::Type::Video;
      if (s == "image")  return Entry::Type::Image;
      return Entry::Type::Other;
    }

    const char* TypeToString(Entry::Type type) {
      switch (type) {
      case Entry::Type::Folder: return "folder";
      case Entry::Type::Audio:  return "audio";
      case Entry::Type::Video:  return "video";
      case Entry::Type::Image:  return "image";
      default:                  return "other";
      }
    }

    // Converts one entry object into 'out'. The script writes the same six
    // fields the app reads; 'id' defaults to the uri, then to the name, so
    // a plugin listing plain streams need not invent one.
    bool ToEntry(JSContext* ctx, JSValue v, Entry& out, std::string& error) {
      if (!JS_IsObject(v)) {
	error = "entry is not an object";
	return false;
      }

      out.name = FieldString(ctx, v, "name");
      if (out.name.empty()) {
	error = "entry has no name";
	return false;
      }
      out.type = TypeFromString(FieldString(ctx, v, "type"));
      out.description = FieldString(ctx, v, "description");
      out.image = FieldString(ctx, v, "image");
      out.uri = FieldString(ctx, v, "uri");
      out.id = FieldString(ctx, v, "id",
			   out.uri.empty() ? out.name : out.uri);
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
	  JS_SetPropertyStr(ctx, obj, key,
			    JS_NewStringLen(ctx, value.data(), value.size()));
      };
      set_str("id", e.id);
      JS_SetPropertyStr(ctx, obj, "type", JS_NewString(ctx, TypeToString(e.type)));
      set_str("name", e.name);
      set_str("description", e.description);
      set_str("image", e.image);
      set_str("uri", e.uri);
      return obj;
    }

    // Converts the array of entry objects returned by browse().
    bool ToListing(JSContext* ctx, JSValue v, Listing& out,
		   std::string& error) {
      int64_t n = 0;
      if (!ArrayLength(ctx, v, n)) {
	error = IsThenable(ctx, v)
	  ? "browse() returned a promise; plugin functions must be synchronous"
	  : "browse() did not return an array";
	return false;
      }

      for (int64_t i = 0; i < n; i++) {
	Ref item(ctx, JS_GetPropertyUint32(ctx, v, (uint32_t)i));
	if (JS_IsException(item.get())) {
	  error = TakeException(ctx);
	  return false;
	}
	Entry e;
	if (!ToEntry(ctx, item.get(), e, error)) {
	  error = "entry " + std::to_string(i + 1) + ": " + error;
	  return false;
	}
	out.push_back(std::move(e));
      }
      return true;
    }

    // console.log()/warn()/error(); the only global a plugin gets beyond
    // the standard objects. Output goes to the RmlUi log, tagged with the
    // provider name.
    JSValue ConsoleWrite(JSContext* ctx, JSValue this_val, int argc,
			 JSValue* argv, int magic) {
      (void)this_val;

      std::string line;
      for (int i = 0; i < argc; i++) {
	if (i)
	  line += ' ';
	line += ToStdString(ctx, argv[i]);
      }

      Rml::Log::Type level = Rml::Log::LT_INFO;
      if (magic == 1) level = Rml::Log::LT_WARNING;
      else if (magic == 2) level = Rml::Log::LT_ERROR;

      const JsPlugin* plugin =
	static_cast<const JsPlugin*>(JS_GetContextOpaque(ctx));
      Rml::Log::Message(level, "[%s] %s",
			plugin ? plugin->ProviderName().c_str() : "plugin",
			line.c_str());
      return JS_UNDEFINED;
    }

    void InstallConsole(JSContext* ctx) {
      Ref global(ctx, JS_GetGlobalObject(ctx));
      JSValue console = JS_NewObject(ctx);
      static const struct { const char* name; int magic; } kMethods[] = {
	{"log", 0}, {"info", 0}, {"debug", 0}, {"warn", 1}, {"error", 2},
      };
      for (const auto& m : kMethods)
	JS_SetPropertyStr(ctx, console, m.name,
			  JS_NewCFunctionMagic(ctx, ConsoleWrite, m.name, 1,
					       JS_CFUNC_generic_magic,
					       m.magic));
      JS_SetPropertyStr(ctx, global.get(), "console", console);
    }

    bool ReadFile(const std::string& path, std::string& out) {
      std::ifstream file(path, std::ios::binary);
      if (!file)
	return false;
      std::ostringstream buffer;
      buffer << file.rdbuf();
      out = buffer.str();
      return true;
    }
  }

  int JsPlugin::InterruptHandler(JSRuntime* rt, void* opaque) {
    (void)rt;
    JsPlugin* self = static_cast<JsPlugin*>(opaque);
    if (!self->armed_)
      return 0;
    return std::chrono::steady_clock::now() >= self->deadline_ ? 1 : 0;
  }

  void JsPlugin::ArmWatchdog(int timeout_ms) {
    deadline_ = std::chrono::steady_clock::now() +
      std::chrono::milliseconds(timeout_ms);
    armed_ = true;
  }

  void JsPlugin::DisarmWatchdog() {
    armed_ = false;
  }

  void JsPlugin::ExtendDeadline(std::chrono::steady_clock::duration by) {
    if (armed_)
      deadline_ += by;
  }

  JsPlugin::JsPlugin(const std::string& path)
    : provider(JS_UNDEFINED), name_(BaseName(path)) {
    std::string source;
    if (!ReadFile(path, source)) {
      load_error_ = "cannot read " + path;
      return;
    }

    if (!(rt = JS_NewRuntime())) {
      load_error_ = "cannot create JS runtime";
      return;
    }
    JS_SetMemoryLimit(rt, kMemoryLimitBytes);
    JS_SetInterruptHandler(rt, &JsPlugin::InterruptHandler, this);

    if (!(ctx = JS_NewContext(rt))) {
      load_error_ = "cannot create JS context";
      return;
    }
    JS_SetContextOpaque(ctx, this);
    http_ = std::make_unique<net::HttpClient>();
    InstallConsole(ctx);
    InstallHttp(ctx);

    // The script body is wrapped in a function so a plugin can 'return' its
    // provider object, the way a CommonJS module does. The prologue has no
    // newline in it, so reported line numbers still match the file.
    const std::string wrapped = "(function(){" + source + "\n})();";

    CallScope scope(*this);
    Ref result(ctx, JS_Eval(ctx, wrapped.c_str(), wrapped.size(),
			    BaseName(path).c_str(), JS_EVAL_TYPE_GLOBAL));
    if (JS_IsException(result.get())) {
      load_error_ = TakeException(ctx);
      return;
    }
    if (!JS_IsObject(result.get())) {
      load_error_ = "script did not return a provider object";
      return;
    }

    provider = result.release();
    const std::string name = FieldString(ctx, provider, "name");
    if (!name.empty())
      name_ = name;
  }

  JsPlugin::~JsPlugin() {
    if (ctx) {
      JS_FreeValue(ctx, provider);
      JS_FreeContext(ctx);
    }
    if (rt)
      JS_FreeRuntime(rt);
  }

  JsSource::JsSource(JsPluginPtr plugin, JSValue source, std::string name,
		     std::string detail, std::string icon, std::string root)
    : plugin_(std::move(plugin)), source_(source),
      name_(std::move(name)), detail_(std::move(detail)),
      icon_(std::move(icon)), root_(std::move(root)) {}

  JsSource::~JsSource() {
    std::lock_guard<std::mutex> lock(plugin_->mutex);
    JS_FreeValue(plugin_->ctx, source_);
  }

  bool JsSource::Browse(const std::string& id, Listing& out,
			std::string& error) {
    std::lock_guard<std::mutex> lock(plugin_->mutex);
    JSContext* ctx = plugin_->ctx;
    CallScope scope(*plugin_);

    Ref browse(ctx, JS_GetPropertyStr(ctx, source_, "browse"));
    if (!JS_IsFunction(ctx, browse.get())) {
      error = "source has no browse function";
      return false;
    }

    Ref arg(ctx, JS_NewStringLen(ctx, id.data(), id.size()));
    JSValue argv[] = {arg.get()};
    // 'this' is the source object, so a plugin can keep state on it.
    Ref result(ctx, JS_Call(ctx, browse.get(), source_, 1, argv));
    if (JS_IsException(result.get())) {
      error = TakeException(ctx);
      return false;
    }

    return ToListing(ctx, result.get(), out, error);
  }

  bool JsSource::Resolve(const Entry& entry, std::string& uri,
			 std::string& error) {
    std::lock_guard<std::mutex> lock(plugin_->mutex);
    JSContext* ctx = plugin_->ctx;
    CallScope scope(*plugin_);

    Ref resolve(ctx, JS_GetPropertyStr(ctx, source_, "resolve"));
    if (!JS_IsFunction(ctx, resolve.get())) {
      // No resolve(): the uri from browse() is all there is.
      uri = entry.uri;
      if (uri.empty()) {
	error = "this item has nothing to play";
	return false;
      }
      return true;
    }

    Ref id(ctx, JS_NewStringLen(ctx, entry.id.data(), entry.id.size()));
    Ref obj(ctx, EntryToJs(ctx, entry));
    JSValue argv[] = {id.get(), obj.get()};
    Ref result(ctx, JS_Call(ctx, resolve.get(), source_, 2, argv));
    if (JS_IsException(result.get())) {
      error = TakeException(ctx);
      return false;
    }

    // Either the uri itself, or an entry object like the ones browse()
    // returns. Anything else means the plugin gave up on this item.
    if (IsThenable(ctx, result.get())) {
      error = "resolve() returned a promise; plugin functions must be "
	"synchronous";
      return false;
    }
    if (JS_IsString(result.get()))
      uri = ToStdString(ctx, result.get());
    else if (JS_IsObject(result.get()))
      uri = FieldString(ctx, result.get(), "uri");

    if (uri.empty()) {
      error = "resolve() did not return a uri";
      return false;
    }
    return true;
  }

  JsProvider::JsProvider(const std::string& path)
    : plugin_(std::make_shared<JsPlugin>(path)) {}

  bool JsProvider::Discover(std::vector<SourcePtr>& out, std::string& error) {
    if (!plugin_->LoadError().empty()) {
      error = plugin_->LoadError();
      return false;
    }

    std::lock_guard<std::mutex> lock(plugin_->mutex);
    JSContext* ctx = plugin_->ctx;
    CallScope scope(*plugin_);

    Ref discover(ctx, JS_GetPropertyStr(ctx, plugin_->provider, "discover"));
    if (!JS_IsFunction(ctx, discover.get())) {
      error = "plugin has no discover function";
      return false;
    }

    Ref result(ctx, JS_Call(ctx, discover.get(), plugin_->provider, 0,
			    nullptr));
    if (JS_IsException(result.get())) {
      error = TakeException(ctx);
      return false;
    }

    int64_t n = 0;
    if (!ArrayLength(ctx, result.get(), n)) {
      error = IsThenable(ctx, result.get())
	? "discover() returned a promise; plugin functions must be synchronous"
	: "discover() did not return an array";
      return false;
    }

    std::vector<SourcePtr> found;
    for (int64_t i = 0; i < n; i++) {
      Ref item(ctx, JS_GetPropertyUint32(ctx, result.get(), (uint32_t)i));
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
      const std::string detail =
	FieldString(ctx, item.get(), "detail", plugin_->ProviderName());
      const std::string icon =
	FieldString(ctx, item.get(), "icon", "\U0001F9E9");
      const std::string root = FieldString(ctx, item.get(), "root", "/");

      found.push_back(std::make_shared<JsSource>(plugin_, item.release(), name,
						 detail, icon, root));
    }

    out.insert(out.end(), std::make_move_iterator(found.begin()),
	       std::make_move_iterator(found.end()));
    return true;
  }

  void LoadJsProviders(const std::string& dir,
		       std::vector<std::unique_ptr<Provider>>& out) {
    DIR* d = ::opendir(dir.c_str());
    if (!d)
      return; // no plugin directory, no plugins

    std::vector<std::string> scripts;
    while (struct dirent* de = ::readdir(d)) {
      const std::string name = de->d_name;
      if (name.size() > 3 && name[0] != '.' &&
	  name.compare(name.size() - 3, 3, ".js") == 0)
	scripts.push_back(dir + "/" + name);
    }
    ::closedir(d);

    std::sort(scripts.begin(), scripts.end());
    for (const std::string& path : scripts)
      out.push_back(std::make_unique<JsProvider>(path));
  }
}
