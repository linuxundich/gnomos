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
  TaskQueue() : worker_([this] { Run(); }) {}

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
      task();
    }
  }

  std::mutex mutex_;
  std::condition_variable cv_;
  std::deque<std::function<void()>> tasks_;
  bool stopping_ = false;
  // Must be initialized last so it doesn't start running before the other
  // members it might reference (via the tasks pushed onto it) exist.
  std::thread worker_;
};

}  // namespace gnomos
