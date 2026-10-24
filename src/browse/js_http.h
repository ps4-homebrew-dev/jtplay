// SPDX-License-Identifier: GPL-3.0-or-later
//
// The 'http' global available to plugin scripts. The transport itself is
// net::HttpClient (see net/http_client.h), shared with the artwork cache;
// this header is only the QuickJS binding on top of it. Requests are
// blocking: plugins run on the app's worker thread, which is already where
// the DLNA source does its blocking I/O.

#pragma once

#include <quickjs.h>

#include "net/http_client.h"

namespace browse {

  // Installs 'http' (get/post/request) on the context's global object. The
  // client comes from the JsPlugin behind JS_GetContextOpaque().
  void InstallHttp(JSContext* ctx);

}
