// SPDX-License-Identifier: GPL-3.0-or-later
//
// One background thread running tasks strictly in the order they were
// posted. The app keeps a few of these as lanes, one per kind of blocking
// work, so that a slow server in one lane never holds up another: stopping
// playback does not queue behind a folder that is taking its time.
#ifndef UTIL_WORKER_H
#define UTIL_WORKER_H

#include <condition_variable>
#include <cstdio>
#include <deque>
#include <exception>
#include <functional>
#include <mutex>
#include <thread>

namespace util {

class Worker {
public:
  Worker() = default;
  ~Worker() { Stop(); }

  Worker(const Worker&) = delete;
  Worker& operator=(const Worker&) = delete;

  void Start() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (running_)
      return;
    running_ = true;
    thread_ = std::thread(&Worker::Main, this);
  }

  // Drops what has not started yet, lets the task that is running finish,
  // and joins the thread. Tasks posted from here on are dropped as well.
  void Stop() {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (!running_)
        return;
      running_ = false;
      tasks_.clear();
    }
    cv_.notify_all();
    if (thread_.joinable())
      thread_.join();
  }

  // Any thread. False (and the task is dropped) once stopped.
  bool Post(std::function<void()> task) {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (!running_)
        return false;
      tasks_.push_back(std::move(task));
    }
    cv_.notify_one();
    return true;
  }

private:
  void Main() {
    for (;;) {
      std::function<void()> task;
      {
        std::unique_lock<std::mutex> lock(mutex_);
        cv_.wait(lock, [&] { return !tasks_.empty() || !running_; });
        if (!running_)
          return;
        task = std::move(tasks_.front());
        tasks_.pop_front();
      }
      // A task that throws must not take the process down with it (an
      // uncaught exception on a thread is std::terminate): what it was
      // doing is lost, the lane goes on.
      try {
        task();
      } catch (const std::exception& e) {
        std::fprintf(stderr, "worker: task failed: %s\n", e.what());
      } catch (...) {
        std::fprintf(stderr, "worker: task failed\n");
      }
    }
  }

  std::mutex mutex_;
  std::condition_variable cv_;
  std::deque<std::function<void()>> tasks_;
  std::thread thread_;
  bool running_ = false;
};

} // namespace util

#endif
