// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <list>
#include <string>
#include <unordered_map>

#include <gdkmm/texture.h>
#include <glibmm/bytes.h>
#include <glibmm/refptr.h>

namespace gnomos
{

// Process-wide, disk-backed LRU cache of decoded cover art textures, keyed
// by the resolved art_uri. Every list row (CoverThumbnail) and the Now
// Playing panel (PlayerBar) independently need the same handful of album
// arts over and over — the same album showing up multiple times in a
// queue, or a list simply being rebuilt (Clear() + re-render) on every
// refresh — so without an in-memory cache, the exact same image gets
// re-fetched over HTTP and re-decoded every single time; without the disk
// half, all of that work is lost again on every app restart. Main-thread
// only: every user here already only touches it from Gio async callbacks
// dispatched onto the GTK main loop, so no locking is needed. Disk reads
// here are synchronous (not async like the original HTTP fetch) — local
// disk I/O for a small cached image is fast enough not to visibly block
// the UI, unlike the network fetch this is standing in for.
class ArtCache
{
public:
  static ArtCache& Instance();

  // Memory cache only (no disk fallback) — used by SetArtUri()-style
  // callers to decide whether an HTTP fetch is even needed. Returns an
  // empty RefPtr (falsy) on a miss.
  Glib::RefPtr<Gdk::Texture> Get(const std::string& uri);

  // Decodes raw_bytes, stores it (memory + disk, evicting older disk
  // entries if this pushes total usage over GetMaxDiskMb()), and returns
  // the decoded texture — an empty RefPtr if raw_bytes couldn't be
  // decoded as an image at all.
  Glib::RefPtr<Gdk::Texture> Put(const std::string& uri, const Glib::RefPtr<Glib::Bytes>& raw_bytes);

  // Persisted setting (see art-cache.ini next to state.ini/linked-services.ini
  // under $XDG_CONFIG_HOME/gnomos/). Setting a smaller limit than the
  // current on-disk usage evicts immediately.
  unsigned GetMaxDiskMb() const { return max_disk_mb_; }
  void SetMaxDiskMb(unsigned mb);

  // Wipes both the in-memory cache and every file in the on-disk cache
  // directory.
  void Clear();

  // Current on-disk usage, for display in a settings UI.
  std::uint64_t GetDiskUsageBytes() const;

private:
  ArtCache();

  static std::string CacheDir();
  std::string PathFor(const std::string& uri) const;
  void InsertMemory(const std::string& uri, const Glib::RefPtr<Gdk::Texture>& texture);
  void Touch(const std::string& uri);
  void EnforceDiskLimit();
  void LoadSettings();
  void SaveSettings() const;

  struct Entry
  {
    Glib::RefPtr<Gdk::Texture> texture;
    std::list<std::string>::iterator lru_it;
  };

  // Bounds the in-memory half only — small and fixed, since decoded
  // textures are cheap and a session rarely has more than a few hundred
  // distinct ones in view at once. The user-configurable, persisted limit
  // (max_disk_mb_) is what actually matters for "how much does this cache
  // cost you", since that's the part that survives a restart.
  static constexpr size_t kMaxMemoryEntries = 300;

  std::unordered_map<std::string, Entry> entries_;
  std::list<std::string> lru_;  // most-recently-used at the front

  unsigned max_disk_mb_ = 200;
};

}  // namespace gnomos
