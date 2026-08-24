// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>

namespace gnomos
{

// libnoson's actions (Player::Play(), ::SetVolume(), System::Discover(), a
// ContentDirectory::Browse() call, ...) are synchronous, blocking SOAP/HTTP
// calls (see noson/src/sonosplayer.cpp, sonossystem.cpp, contentdirectory.cpp).
// They must never run on the GTK main thread. TaskQueue runs them one at a
// time, in submission order, on a single dedicated worker thread, so two
// user actions (e.g. rapid volume-slider drags) can never race each other
// on the same underlying connection.
class TaskQueue
{
public:
  // on_busy_changed, if given, fires on this queue's own worker thread —
  // any caller touching GTK/main-thread-only state from it needs its own
  // marshaling (a Glib::Dispatcher, same as every other cross-thread
  // notification in this codebase). Called with true right before the
  // first task of a new burst starts, and false only once the queue is
  // genuinely empty again afterward — not once per task — so a rapid
  // sequence of back-to-back actions (e.g. a fast volume drag) reads as
  // one continuous busy period rather than flickering on/off between
  // each individual task.
  explicit TaskQueue(std::function<void(bool)> on_busy_changed = {})
  : on_busy_changed_(std::move(on_busy_changed)), worker_([this] { Run(); })
  {
  }

  ~TaskQueue()
  {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      stopping_ = true;
    }
    cv_.notify_all();
    worker_.join();
  }

  TaskQueue(const TaskQueue&) = delete;
  TaskQueue& operator=(const TaskQueue&) = delete;

  void Push(std::function<void()> task)
  {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      tasks_.push_back(std::move(task));
    }
    cv_.notify_all();
  }

private:
  void Run()
  {
    for (;;)
    {
      std::function<void()> task;
      {
        std::unique_lock<std::mutex> lock(mutex_);
        cv_.wait(lock, [this] { return stopping_ || !tasks_.empty(); });
        if (tasks_.empty())
        {
          if (stopping_)
            return;
          continue;
        }
        task = std::move(tasks_.front());
        tasks_.pop_front();
      }
      if (on_busy_changed_)
        on_busy_changed_(true);
      task();
      bool more_pending;
      {
        std::lock_guard<std::mutex> lock(mutex_);
        more_pending = !tasks_.empty();
      }
      if (!more_pending && on_busy_changed_)
        on_busy_changed_(false);
    }
  }

  std::function<void(bool)> on_busy_changed_;
  std::mutex mutex_;
  std::condition_variable cv_;
  std::deque<std::function<void()>> tasks_;
  bool stopping_ = false;
  // Must be initialized last so it doesn't start running before the other
  // members it might reference (via the tasks pushed onto it) exist.
  std::thread worker_;
};

}  // namespace gnomos
