// SPDX-License-Identifier: GPL-3.0-or-later
//
// Small helpers for talking to QuickJS, shared by the files that make up
// the plugin host. Internal to src/browse/js_*.cpp.
//
// Only the QuickJS API that every fork has is used here and in those files
// (Bellard's releases, quickjs-ng, the console SDK's port): where two of
// them disagree on a function, it is avoided or duck-typed around.
#ifndef BROWSE_JS_UTIL_H
#define BROWSE_JS_UTIL_H

#include <cmath>
#include <cstdint>
#include <string>

#include <quickjs.h>

namespace browse {
namespace js {

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

inline JSValue NewString(JSContext* ctx, const std::string& s) {
  return JS_NewStringLen(ctx, s.data(), s.size());
}

// Drops the pending exception, for where a failure only means "absent".
inline void ClearException(JSContext* ctx) {
  JS_FreeValue(ctx, JS_GetException(ctx));
}

// 'v' as a string; "" when the conversion throws (a Symbol, a throwing
// toString).
inline std::string ToStdString(JSContext* ctx, JSValue v) {
  size_t len = 0;
  const char* s = JS_ToCStringLen(ctx, &len, v);
  if (!s) {
    ClearException(ctx);
    return {};
  }
  std::string out(s, len);
  JS_FreeCString(ctx, s);
  return out;
}

// The same for a function argument: false leaves the exception pending, for
// the caller to return JS_EXCEPTION.
inline bool StringArg(JSContext* ctx, JSValue v, std::string& out) {
  size_t len = 0;
  const char* s = JS_ToCStringLen(ctx, &len, v);
  if (!s)
    return false;
  out.assign(s, len);
  JS_FreeCString(ctx, s);
  return true;
}

// Throws a plain Error, the way a plugin author expects to catch it.
inline JSValue ThrowError(JSContext* ctx, const std::string& message) {
  JSValue error = JS_NewError(ctx);
  JS_SetPropertyStr(ctx, error, "message", NewString(ctx, message));
  return JS_Throw(ctx, error);
}

// Takes the pending exception and renders it as one line. Errors carry
// the script location in their stack, so the first stack frame is worth
// keeping: "SyntaxError: unexpected token (sr.js:12)".
inline std::string TakeException(JSContext* ctx) {
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

// Field readers; all tolerate a missing field ('obj' is the object).

// A string field. Numbers are convenient for ids, so they count; anything
// else (objects, booleans, null, undefined) is treated as absent rather
// than stringified.
inline std::string FieldString(JSContext* ctx, JSValue obj, const char* key,
                               const std::string& fallback = {}) {
  Ref v(ctx, JS_GetPropertyStr(ctx, obj, key));
  if (JS_IsException(v.get())) {
    ClearException(ctx);
    return fallback;
  }
  if (!JS_IsString(v.get()) && !JS_IsNumber(v.get()))
    return fallback;
  return ToStdString(ctx, v.get());
}

// A finite number field, or 'fallback' when absent or not a number.
inline double FieldNumber(JSContext* ctx, JSValue obj, const char* key, double fallback) {
  Ref v(ctx, JS_GetPropertyStr(ctx, obj, key));
  if (JS_IsException(v.get())) {
    ClearException(ctx);
    return fallback;
  }
  double d = 0;
  if (!JS_IsNumber(v.get()) || JS_ToFloat64(ctx, &d, v.get()) < 0 || !std::isfinite(d))
    return fallback;
  return d;
}

inline bool FieldBool(JSContext* ctx, JSValue obj, const char* key, bool fallback) {
  Ref v(ctx, JS_GetPropertyStr(ctx, obj, key));
  if (JS_IsException(v.get())) {
    ClearException(ctx);
    return fallback;
  }
  return JS_IsBool(v.get()) ? JS_ToBool(ctx, v.get()) != 0 : fallback;
}

// Whether 'obj' has a function called 'key'.
inline bool HasFunction(JSContext* ctx, JSValue obj, const char* key) {
  Ref v(ctx, JS_GetPropertyStr(ctx, obj, key));
  if (JS_IsException(v.get())) {
    ClearException(ctx);
    return false;
  }
  return JS_IsFunction(ctx, v.get());
}

// Length of an array (or any array-like). JS_IsArray() has an
// incompatible signature across QuickJS forks, so duck-type instead.
inline bool ArrayLength(JSContext* ctx, JSValue v, int64_t& out) {
  if (!JS_IsObject(v))
    return false;
  Ref len(ctx, JS_GetPropertyStr(ctx, v, "length"));
  if (!JS_IsNumber(len.get()))
    return false;
  if (JS_ToInt64(ctx, &out, len.get()) < 0) {
    ClearException(ctx);
    return false;
  }
  return out >= 0;
}

// Calls 'visit(name, value)' for every own enumerable string-keyed property
// of 'obj'; 'value' is borrowed. False (exception pending) if enumeration or
// a getter threw.
template <typename Visit>
bool ForEachProperty(JSContext* ctx, JSValue obj, Visit&& visit) {
  JSPropertyEnum* props = nullptr;
  uint32_t count = 0;
  if (JS_GetOwnPropertyNames(ctx, &props, &count, obj, JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY) < 0)
    return false;

  bool ok = true;
  for (uint32_t i = 0; i < count && ok; i++) {
    const char* name = JS_AtomToCString(ctx, props[i].atom);
    JSValue value = JS_GetProperty(ctx, obj, props[i].atom);
    if (!name || JS_IsException(value))
      ok = false;
    else
      ok = visit(name, value);
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

} // namespace js
} // namespace browse

#endif
