// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace gnomos
{

// Runs image-decode jobs (ArtCache::DecodeScaledTexture(), a pure,
// stateless call — see its own comment) off the GTK main thread, across a
// small fixed pool of worker threads. Confirmed live: decoding a cached
// cover art image on this system costs ~6ms each — apparently a sandboxed
// glycin loader round trip, not raw pixel work — which is unnoticeable for
// one image but added up to several *seconds* of frozen UI when a large
// grid (bonob's/the local library's own "Albums", 1000+ entries) rebuilt
// synchronously on the main thread. Unlike TaskQueue (deliberately single-
// threaded, since libnoson calls must be serialized against the same
// connection), decode jobs are fully independent of each other and safe to
// run concurrently — a pool, not a single worker, is what actually cuts
// the wall-clock time for a large grid rather than just relocating it.
class ArtDecodePool
{
public:
  static ArtDecodePool& Instance();

  void Push(std::function<void()> job);

  ArtDecodePool(const ArtDecodePool&) = delete;
  ArtDecodePool& operator=(const ArtDecodePool&) = delete;

private:
  ArtDecodePool();
  ~ArtDecodePool();
  void Run();

  std::mutex mutex_;
  std::condition_variable cv_;
  std::deque<std::function<void()>> jobs_;
  bool stopping_ = false;
  // Must be initialized last so workers don't start running before the
  // other members they touch (jobs_, mutex_, cv_) exist.
  std::vector<std::thread> workers_;
};

}  // namespace gnomos
