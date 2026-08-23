// SPDX-License-Identifier: GPL-3.0-or-later

#include "art-decode-pool.h"

#include <algorithm>

namespace gnomos
{

ArtDecodePool& ArtDecodePool::Instance()
{
  static ArtDecodePool instance;
  return instance;
}

ArtDecodePool::ArtDecodePool()
{
  // A handful of threads is enough to hide per-image IPC/decode latency
  // behind concurrency without spawning hundreds of OS threads for a
  // 1000+-entry grid; clamped to a sane range in case
  // hardware_concurrency() ever returns something silly (0 is documented
  // as possible when it can't be determined).
  unsigned n = std::thread::hardware_concurrency();
  if (n < 2)
    n = 2;
  if (n > 4)
    n = 4;
  for (unsigned i = 0; i < n; ++i)
    workers_.emplace_back([this] { Run(); });
}

ArtDecodePool::~ArtDecodePool()
{
  {
    std::lock_guard<std::mutex> lock(mutex_);
    stopping_ = true;
  }
  cv_.notify_all();
  for (std::thread& worker : workers_)
    worker.join();
}

void ArtDecodePool::Push(std::function<void()> job)
{
  {
    std::lock_guard<std::mutex> lock(mutex_);
    jobs_.push_back(std::move(job));
  }
  cv_.notify_one();
}

void ArtDecodePool::Run()
{
  for (;;)
  {
    std::function<void()> job;
    {
      std::unique_lock<std::mutex> lock(mutex_);
      cv_.wait(lock, [this] { return stopping_ || !jobs_.empty(); });
      if (jobs_.empty())
      {
        if (stopping_)
          return;
        continue;
      }
      job = std::move(jobs_.front());
      jobs_.pop_front();
    }
    job();
  }
}

}  // namespace gnomos
