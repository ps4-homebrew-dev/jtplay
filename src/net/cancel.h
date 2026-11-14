// SPDX-License-Identifier: GPL-3.0-or-later
//
// Giving up on blocking work. Whoever starts a request that may block (a
// browse, a search, the resolve before playback) makes a token and keeps
// it; the thread doing the work puts it in scope, and everything that waits
// underneath (an HTTP transfer, a running plugin script) asks Cancelled()
// now and then and unwinds with an error once it says yes.
//
// The token rides on the thread rather than through every signature in
// between: what is cancelled is "whatever this thread is doing for that
// request", and the layers in between (a Source, a plugin's script) have
// nothing to add to it.
#ifndef NET_CANCEL_H
#define NET_CANCEL_H

#include <atomic>
#include <memory>

namespace net {

using CancelToken = std::shared_ptr<std::atomic<bool>>;

inline CancelToken MakeCancelToken() {
  return std::make_shared<std::atomic<bool>>(false);
}

// Any thread. A null token is fine: nothing to cancel.
inline void Cancel(const CancelToken& token) {
  if (token)
    token->store(true);
}

namespace detail {
// The token in scope on the calling thread.
inline const std::atomic<bool>*& CurrentCancel() {
  static thread_local const std::atomic<bool>* current = nullptr;
  return current;
}
} // namespace detail

// Makes 'token' the one Cancelled() answers for on this thread, for as long
// as the scope lives. Scopes nest; the innermost one counts.
class CancelScope {
public:
  explicit CancelScope(const CancelToken& token)
    : token_(token), outer_(detail::CurrentCancel()) {
    detail::CurrentCancel() = token_.get();
  }
  ~CancelScope() { detail::CurrentCancel() = outer_; }

  CancelScope(const CancelScope&) = delete;
  CancelScope& operator=(const CancelScope&) = delete;

private:
  CancelToken token_; // kept alive while in scope
  const std::atomic<bool>* outer_;
};

// Whether the work this thread is doing has been given up on. Cheap; meant
// to be polled from progress callbacks and interrupt handlers.
inline bool Cancelled() {
  const std::atomic<bool>* flag = detail::CurrentCancel();
  return flag && flag->load(std::memory_order_relaxed);
}

// What a cancelled operation reports as its error, so callers can tell it
// from a failure worth showing.
inline constexpr const char* kCancelledError = "cancelled";

} // namespace net

#endif
