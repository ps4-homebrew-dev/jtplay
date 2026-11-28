// SPDX-License-Identifier: GPL-3.0-or-later
//
// See js_plugin.h.
#include "browse/js_plugin.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

#include <sys/stat.h>

#include <tinyxml2.h>

#include "browse/js_prelude.h"
#include "browse/js_util.h"
#include "net/cancel.h"
#include "util/atomic_file.h"

namespace browse {
namespace {

using namespace js;

// Script time per call; waiting on the network does not count (see
// ExtendDeadline()), so this only has to catch a runaway loop.
constexpr int kCallTimeoutMs = 10000;
// A plugin's storage is for ids, tokens and choices, not for data.
constexpr size_t kStorageMaxBytes = 1u << 20;
// xml.parse(): deeper than any feed or manifest, shallow enough for the
// worker thread's stack.
constexpr int kXmlMaxDepth = 200;
// Promise jobs run for one call before it is called a loop.
constexpr int kMaxPendingJobs = 100000;

std::string BaseName(const std::string& path) {
  const size_t slash = path.rfind('/');
  return slash == std::string::npos ? path : path.substr(slash + 1);
}

JsPlugin* PluginFor(JSContext* ctx) {
  return static_cast<JsPlugin*>(JS_GetContextOpaque(ctx));
}

// True for anything that looks like a promise.
bool IsThenable(JSContext* ctx, JSValue v) {
  return JS_IsObject(v) && HasFunction(ctx, v, "then");
}

// An error value as the app shows it: what went wrong, in the thrower's
// words. A plain Error is a plugin saying so on purpose ("not available
// from here"), and its message is all of it; any other kind (TypeError,
// ReferenceError, ...) is a bug in the script and keeps its name.
//
// 'where' (if given) gets the place it was thrown from, for the log: the
// first stack frame that is in the plugin's own script rather than in the
// host's prelude or a native function.
std::string DescribeError(JSContext* ctx, JSValue e, std::string* where = nullptr) {
  std::string msg;
  if (JS_IsObject(e)) {
    const std::string name = FieldString(ctx, e, "name");
    msg = FieldString(ctx, e, "message");
    if (msg.empty())
      msg = ToStdString(ctx, e);
    else if (!name.empty() && name != "Error")
      msg = name + ": " + msg;

    Ref stack(ctx, JS_GetPropertyStr(ctx, e, "stack"));
    if (where && JS_IsString(stack.get())) {
      const std::string frames = ToStdString(ctx, stack.get());
      size_t at = 0;
      while (at < frames.size()) {
        const size_t end = std::min(frames.find('\n', at), frames.size());
        std::string frame = frames.substr(at, end - at);
        at = end + 1;
        const size_t begin = frame.find_first_not_of(" \t");
        if (begin == std::string::npos)
          continue;
        frame = frame.substr(begin);
        if (frame.find("<jtplay>") != std::string::npos ||
            frame.find("(native)") != std::string::npos)
          continue;
        *where = frame;
        break;
      }
    }
  } else {
    msg = ToStdString(ctx, e);
  }
  return msg.empty() ? "(unknown error)" : msg;
}

// --- console ---------------------------------------------------------------

JSValue ConsoleWrite(JSContext* ctx, JSValue this_val, int argc, JSValue* argv, int magic) {
  (void)this_val;
  std::string line;
  for (int i = 0; i < argc; i++) {
    if (i)
      line += ' ';
    // Objects as JSON: "[object Object]" tells a plugin author nothing.
    // (Errors, told by their stack, say more as they print themselves.)
    if (JS_IsObject(argv[i]) && !JS_IsFunction(ctx, argv[i]) &&
        FieldString(ctx, argv[i], "stack", "\x01") == "\x01") {
      Ref json(ctx, JS_JSONStringify(ctx, argv[i], JS_UNDEFINED, JS_UNDEFINED));
      if (JS_IsString(json.get())) {
        line += ToStdString(ctx, json.get());
        continue;
      }
      if (JS_IsException(json.get()))
        ClearException(ctx);
    }
    line += ToStdString(ctx, argv[i]);
  }
  if (const JsPlugin* plugin = PluginFor(ctx))
    plugin->Log(magic == 2 ? JsLogLevel::Error : magic == 1 ? JsLogLevel::Warning
                                                           : JsLogLevel::Info, line);
  return JS_UNDEFINED;
}

void InstallConsole(JSContext* ctx, JSValue global) {
  JSValue console = JS_NewObject(ctx);
  static const struct {
    const char* name;
    int magic;
  } kMethods[] = {
    {"log", 0}, {"info", 0}, {"debug", 0}, {"warn", 1}, {"error", 2},
  };
  for (const auto& m : kMethods)
    JS_SetPropertyStr(ctx, console, m.name,
                      JS_NewCFunctionMagic(ctx, ConsoleWrite, m.name, 1, JS_CFUNC_generic_magic,
                                           m.magic));
  JS_SetPropertyStr(ctx, global, "console", console);
}

// --- storage ---------------------------------------------------------------

// The stored JSON text; "" when there is none (yet), or nowhere to keep it.
JSValue StorageLoad(JSContext* ctx, JSValue, int, JSValue*) {
  const JsPlugin* plugin = PluginFor(ctx);
  std::string text;
  const std::string path = plugin ? plugin->StoragePath() : std::string();
  if (!path.empty() && !util::ReadFile(path, text, kStorageMaxBytes))
    text.clear();
  return NewString(ctx, text);
}

JSValue StorageSave(JSContext* ctx, JSValue, int argc, JSValue* argv) {
  const JsPlugin* plugin = PluginFor(ctx);
  std::string text;
  if (argc < 1 || !StringArg(ctx, argv[0], text))
    return JS_EXCEPTION;
  if (text.size() > kStorageMaxBytes)
    return ThrowError(ctx, "storage: more than 1 MiB; storage is for small things");
  const std::string path = plugin ? plugin->StoragePath() : std::string();
  if (path.empty())
    return JS_UNDEFINED; // nowhere to keep it: lives as long as the session
  ::mkdir(plugin->Environment().data_dir.c_str(), 0755);
  if (!util::WriteFileAtomic(path, text))
    return ThrowError(ctx, "storage: cannot write " + path);
  return JS_UNDEFINED;
}

// --- settings --------------------------------------------------------------

JSValue SettingGet(JSContext* ctx, JSValue, int argc, JSValue* argv) {
  const JsPlugin* plugin = PluginFor(ctx);
  std::string key;
  if (argc < 1 || !StringArg(ctx, argv[0], key))
    return JS_EXCEPTION;
  return NewString(ctx, plugin ? plugin->SettingValue(key) : std::string());
}

JSValue SettingsRevision(JSContext* ctx, JSValue, int, JSValue*) {
  const JsPlugin* plugin = PluginFor(ctx);
  return JS_NewFloat64(ctx, plugin ? (double)plugin->Environment().settings->Revision() : 0.0);
}

// --- xml -------------------------------------------------------------------

// One element as { name, attrs, children, text }. JS_EXCEPTION when the
// runtime ran out of memory, or the document out of reasonable depth.
JSValue XmlElement(JSContext* ctx, const tinyxml2::XMLElement* element, int depth) {
  if (depth > kXmlMaxDepth)
    return ThrowError(ctx, "xml: nested too deeply");

  Ref node(ctx, JS_NewObject(ctx));
  Ref attrs(ctx, JS_NewObject(ctx));
  Ref children(ctx, JS_NewArray(ctx));
  if (JS_IsException(node.get()) || JS_IsException(attrs.get()) ||
      JS_IsException(children.get()))
    return JS_EXCEPTION;

  for (const tinyxml2::XMLAttribute* a = element->FirstAttribute(); a; a = a->Next())
    if (JS_SetPropertyStr(ctx, attrs.get(), a->Name(), JS_NewString(ctx, a->Value())) < 0)
      return JS_EXCEPTION;

  std::string text;
  uint32_t count = 0;
  for (const tinyxml2::XMLNode* n = element->FirstChild(); n; n = n->NextSibling()) {
    if (const tinyxml2::XMLText* t = n->ToText()) {
      text += t->Value();
    } else if (const tinyxml2::XMLElement* e = n->ToElement()) {
      JSValue child = XmlElement(ctx, e, depth + 1);
      if (JS_IsException(child))
        return JS_EXCEPTION;
      if (JS_SetPropertyUint32(ctx, children.get(), count++, child) < 0)
        return JS_EXCEPTION;
    }
  }

  JS_SetPropertyStr(ctx, node.get(), "name", JS_NewString(ctx, element->Name()));
  JS_SetPropertyStr(ctx, node.get(), "attrs", attrs.release());
  JS_SetPropertyStr(ctx, node.get(), "children", children.release());
  JS_SetPropertyStr(ctx, node.get(), "text", NewString(ctx, text));
  return node.release();
}

JSValue XmlParse(JSContext* ctx, JSValue, int argc, JSValue* argv) {
  std::string source;
  if (argc < 1 || !StringArg(ctx, argv[0], source))
    return JS_EXCEPTION;

  tinyxml2::XMLDocument doc;
  if (doc.Parse(source.data(), source.size()) != tinyxml2::XML_SUCCESS) {
    std::string why = doc.ErrorName() ? doc.ErrorName() : "error";
    if (doc.ErrorLineNum() > 0)
      why += " at line " + std::to_string(doc.ErrorLineNum());
    return ThrowError(ctx, "xml: " + why);
  }
  const tinyxml2::XMLElement* root = doc.RootElement();
  if (!root)
    return ThrowError(ctx, "xml: no root element");
  return XmlElement(ctx, root, 0);
}

// What the prelude builds the script's globals from; it removes the object
// again, so a plugin never sees it.
void InstallNatives(JSContext* ctx, JSValue global, const JsEnvironment& env) {
  JSValue native = JS_NewObject(ctx);
  JS_SetPropertyStr(ctx, native, "storageLoad",
                    JS_NewCFunction(ctx, StorageLoad, "storageLoad", 0));
  JS_SetPropertyStr(ctx, native, "storageSave",
                    JS_NewCFunction(ctx, StorageSave, "storageSave", 1));
  JS_SetPropertyStr(ctx, native, "setting", JS_NewCFunction(ctx, SettingGet, "setting", 1));
  JS_SetPropertyStr(ctx, native, "settingsRevision",
                    JS_NewCFunction(ctx, SettingsRevision, "settingsRevision", 0));
  JS_SetPropertyStr(ctx, native, "xmlParse", JS_NewCFunction(ctx, XmlParse, "xmlParse", 1));
  JS_SetPropertyStr(ctx, native, "api", JS_NewInt32(ctx, kJsApiVersion));
  JS_SetPropertyStr(ctx, native, "version", NewString(ctx, env.app_version));
  JS_SetPropertyStr(ctx, native, "platform", NewString(ctx, env.platform));
  JS_SetPropertyStr(ctx, global, "__native", native);
}

} // namespace

// ---------------------------------------------------------------------------
// JsPlugin
// ---------------------------------------------------------------------------

int JsPlugin::InterruptHandler(JSRuntime*, void* opaque) {
  JsPlugin* self = static_cast<JsPlugin*>(opaque);
  // Nobody is waiting for the answer any more.
  if (net::Cancelled())
    return 1;
  if (!self->armed_)
    return 0;
  if (std::chrono::steady_clock::now() < self->deadline_)
    return 0;
  self->timed_out_ = true;
  return 1;
}

void JsPlugin::ExtendDeadline(std::chrono::steady_clock::duration by) {
  if (armed_)
    deadline_ += by;
}

void JsPlugin::Log(JsLogLevel level, const std::string& text) const {
  if (env_->log) {
    env_->log(level, name_, text);
    return;
  }
  std::fprintf(stderr, "[%s] %s%s\n", name_.c_str(),
               level == JsLogLevel::Error ? "error: "
               : level == JsLogLevel::Warning ? "warning: " : "",
               text.c_str());
}

std::string JsPlugin::StoragePath() const {
  return env_->data_dir.empty() ? std::string() : env_->data_dir + "/" + id_ + ".json";
}

std::string JsPlugin::SettingValue(const std::string& key) const {
  for (const PluginSetting& s : settings_)
    if (s.key == key)
      return env_->settings->Get(id_, key, s.fallback);
  return {};
}

std::string JsPlugin::CallError() {
  std::string where;
  std::string error = DescribeError(ctx, Ref(ctx, JS_GetException(ctx)).get(), &where);
  if (net::Cancelled())
    return net::kCancelledError;
  if (timed_out_)
    return "the plugin took too long (its script ran for more than " +
           std::to_string(kCallTimeoutMs / 1000) + " s)";
  // Where it happened is for whoever wrote the script, not for the screen.
  if (!where.empty())
    Log(JsLogLevel::Error, error + " (" + where + ")");
  return error;
}

bool JsPlugin::Evaluate(const std::string& code, const char* name, JSValue& result,
                        std::string& error) {
  JS_UpdateStackTop(rt);
  deadline_ = std::chrono::steady_clock::now() + std::chrono::milliseconds(kCallTimeoutMs);
  armed_ = true;
  timed_out_ = false;
  result = JS_Eval(ctx, code.c_str(), code.size(), name, JS_EVAL_TYPE_GLOBAL);
  armed_ = false;
  if (JS_IsException(result)) {
    error = CallError();
    result = JS_UNDEFINED;
    return false;
  }
  return true;
}

JsPlugin::JsPlugin(const std::string& path, std::shared_ptr<const JsEnvironment> env)
  : provider(JS_UNDEFINED), env_(std::move(env)), settle_(JS_UNDEFINED), path_(path),
    name_(BaseName(path)) {
  id_ = name_;
  if (id_.size() > 3 && id_.compare(id_.size() - 3, 3, ".js") == 0)
    id_.resize(id_.size() - 3);

  std::string source;
  if (!util::ReadFile(path, source, 4u << 20)) {
    load_error_ = "cannot read " + path;
    return;
  }

  if (!(rt = JS_NewRuntime())) {
    load_error_ = "cannot create JS runtime";
    return;
  }
  JS_SetMemoryLimit(rt, JsPlugin::kMemoryLimitBytes);
  JS_SetInterruptHandler(rt, &JsPlugin::InterruptHandler, this);

  if (!(ctx = JS_NewContext(rt))) {
    load_error_ = "cannot create JS context";
    return;
  }
  JS_SetContextOpaque(ctx, this);
  http_ = std::make_unique<net::HttpClient>();
  {
    Ref global(ctx, JS_GetGlobalObject(ctx));
    InstallConsole(ctx, global.get());
    InstallHttp(ctx);
    InstallNatives(ctx, global.get(), *env_);
  }

  if (!Evaluate(kJsPrelude, "<jtplay>", settle_, load_error_)) {
    load_error_ = "host prelude: " + load_error_;
    return;
  }

  // The script body is wrapped in a function so a plugin can 'return' its
  // provider object, the way a CommonJS module does. The prologue has no
  // newline in it, so reported line numbers still match the file.
  const std::string wrapped = "(function(){" + source + "\n})();";
  JSValue result = JS_UNDEFINED;
  if (!Evaluate(wrapped, name_.c_str(), result, load_error_))
    return;
  if (!JS_IsObject(result) || JS_IsFunction(ctx, result)) {
    JS_FreeValue(ctx, result);
    load_error_ = "script did not return a provider object";
    return;
  }

  provider = result;
  const std::string name = FieldString(ctx, provider, "name");
  if (!name.empty())
    name_ = name;
  ReadSettings();
}

void JsPlugin::Release(JSValue value) {
  std::lock_guard<std::mutex> lock(released_mutex_);
  released_.push_back(value);
}

void JsPlugin::FreeReleased() {
  std::vector<JSValue> values;
  {
    std::lock_guard<std::mutex> lock(released_mutex_);
    values.swap(released_);
  }
  for (JSValue v : values)
    JS_FreeValue(ctx, v);
}

JsPlugin::~JsPlugin() {
  if (ctx) {
    FreeReleased();
    JS_FreeValue(ctx, provider);
    JS_FreeValue(ctx, settle_);
    JS_FreeContext(ctx);
  }
  if (rt)
    JS_FreeRuntime(rt);
}

// The provider's 'settings': an array of
//   { key, label, about, options: ["a", { value: "b", label: "B" }], default }
// or, for a line of text,
//   { key, label, about, type: "text", secret: true, default }
// Anything malformed is left out with a warning rather than failing the
// plugin: a setting is a nicety, the plugin is not.
void JsPlugin::ReadSettings() {
  Ref list(ctx, JS_GetPropertyStr(ctx, provider, "settings"));
  int64_t n = 0;
  if (!ArrayLength(ctx, list.get(), n))
    return;

  for (int64_t i = 0; i < n && i < 32; i++) {
    Ref item(ctx, JS_GetPropertyUint32(ctx, list.get(), (uint32_t)i));
    if (!JS_IsObject(item.get()))
      continue;
    PluginSetting setting;
    setting.key = FieldString(ctx, item.get(), "key");
    setting.label = FieldString(ctx, item.get(), "label", setting.key);
    setting.about = FieldString(ctx, item.get(), "about");

    if (FieldString(ctx, item.get(), "type") == "text") {
      setting.text = true;
      setting.secret = FieldBool(ctx, item.get(), "secret", false);
      setting.fallback = FieldString(ctx, item.get(), "default");
      if (setting.key.empty() || setting.key.find_first_of("=\r\n") != std::string::npos ||
          setting.fallback.find_first_of("\r\n") != std::string::npos) {
        Log(JsLogLevel::Warning, "setting " + std::to_string(i + 1) + " needs a key; ignored");
        continue;
      }
      settings_.push_back(std::move(setting));
      continue;
    }

    Ref options(ctx, JS_GetPropertyStr(ctx, item.get(), "options"));
    int64_t count = 0;
    if (ArrayLength(ctx, options.get(), count)) {
      for (int64_t j = 0; j < count && j < 512; j++) {
        Ref option(ctx, JS_GetPropertyUint32(ctx, options.get(), (uint32_t)j));
        std::string value, label;
        if (JS_IsObject(option.get())) {
          value = FieldString(ctx, option.get(), "value");
          label = FieldString(ctx, option.get(), "label", value);
        } else if (JS_IsString(option.get()) || JS_IsNumber(option.get())) {
          value = label = ToStdString(ctx, option.get());
        }
        // The values end up as lines of the settings file.
        if (value.find_first_of("\r\n") == std::string::npos && !label.empty())
          setting.options.emplace_back(value, label);
      }
    }

    if (setting.key.empty() || setting.key.find_first_of("=\r\n") != std::string::npos ||
        setting.options.size() < 2) {
      Log(JsLogLevel::Warning, "setting " + std::to_string(i + 1) +
                                 " needs a key and at least two options (or type: \"text\");"
                                 " ignored");
      continue;
    }
    setting.fallback = FieldString(ctx, item.get(), "default", setting.options.front().first);
    settings_.push_back(std::move(setting));
  }
}

bool JsPlugin::Call(JSValue object, const char* function, int argc, JSValue* argv,
                    JSValue& result, std::string& error, bool* missing) {
  result = JS_UNDEFINED;
  if (missing)
    *missing = false;
  if (!ctx) {
    error = load_error_.empty() ? "plugin is not loaded" : load_error_;
    return false;
  }

  Ref fn(ctx, JS_GetPropertyStr(ctx, object, function));
  if (!JS_IsFunction(ctx, fn.get())) {
    if (JS_IsException(fn.get()))
      ClearException(ctx);
    if (missing) {
      *missing = true;
      return true;
    }
    error = std::string("the plugin has no ") + function + "() function";
    return false;
  }

  // The script is called from whichever worker thread needs it: tell
  // QuickJS where this one's stack starts, then arm the watchdog.
  JS_UpdateStackTop(rt);
  FreeReleased();
  deadline_ = std::chrono::steady_clock::now() + std::chrono::milliseconds(kCallTimeoutMs);
  armed_ = true;
  timed_out_ = false;

  bool ok = true;
  JSValue value = JS_Call(ctx, fn.get(), object, argc, argv);
  if (JS_IsException(value)) {
    error = CallError();
    value = JS_UNDEFINED;
    ok = false;
  } else if (IsThenable(ctx, value)) {
    ok = Settle(value, function, error);
  }
  armed_ = false;

  if (!ok) {
    JS_FreeValue(ctx, value);
    return false;
  }
  result = value;
  return true;
}

// An async function hands back a promise. There is no event loop to resolve
// it on, but nothing a plugin can wait for needs one: http is synchronous,
// so by the time the function has returned, running the job queue dry is
// all that is left to do.
bool JsPlugin::Settle(JSValue& value, const char* function, std::string& error) {
  Ref box(ctx, JS_NewObject(ctx));
  {
    JSValue args[] = {value, box.get()};
    Ref armed(ctx, JS_Call(ctx, settle_, JS_UNDEFINED, 2, args));
    if (JS_IsException(armed.get())) {
      error = CallError();
      return false;
    }
  }

  for (int jobs = 0;; jobs++) {
    JSContext* job_ctx = nullptr;
    const int rc = JS_ExecutePendingJob(rt, &job_ctx);
    if (rc == 0)
      break;
    if (rc < 0) {
      error = CallError();
      return false;
    }
    if (jobs > kMaxPendingJobs) {
      error = std::string(function) + "() never finished";
      return false;
    }
  }

  if (!FieldBool(ctx, box.get(), "done", false)) {
    error = std::string(function) +
            "() returned a promise that never settles (plugins have no timers to wait on)";
    return false;
  }
  if (FieldBool(ctx, box.get(), "failed", false)) {
    std::string where;
    error = DescribeError(ctx, Ref(ctx, JS_GetPropertyStr(ctx, box.get(), "error")).get(),
                          &where);
    if (net::Cancelled())
      error = net::kCancelledError;
    else if (!where.empty())
      Log(JsLogLevel::Error, error + " (" + where + ")");
    return false;
  }
  JS_FreeValue(ctx, value);
  value = JS_GetPropertyStr(ctx, box.get(), "value");
  return true;
}

} // namespace browse
