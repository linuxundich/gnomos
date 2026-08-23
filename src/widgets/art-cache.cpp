// SPDX-License-Identifier: GPL-3.0-or-later

#include "art-cache.h"

#include <algorithm>
#include <vector>

#include <glib.h>
#include <gdkmm/pixbuf.h>
#include <giomm/file.h>
#include <giomm/fileenumerator.h>
#include <giomm/fileinfo.h>
#include <giomm/memoryinputstream.h>
#include <glibmm/checksum.h>
#include <glibmm/datetime.h>
#include <glibmm/error.h>
#include <glibmm/fileutils.h>
#include <glibmm/keyfile.h>
#include <glibmm/miscutils.h>

namespace gnomos
{

namespace
{
std::string SettingsPath()
{
  return Glib::build_filename(Glib::get_user_config_dir(), "gnomos", "art-cache.ini");
}
}  // namespace

// Decodes raw_bytes scaled so neither dimension exceeds target_size
// (aspect ratio preserved) — via GdkPixbuf's own purpose-built scaling
// loader rather than decoding at full resolution and resizing a
// Gdk::Texture after the fact (texture pixel data is only accessible via
// Gdk::Texture::download(), whose Cairo-ARGB32 layout — premultiplied
// alpha, platform-dependent byte order — would need its own careful,
// easy-to-get-subtly-wrong conversion back to GdkPixbuf's plain
// non-premultiplied RGBA; scaling from the original bytes sidesteps that
// entirely). Returns an empty RefPtr if raw_bytes can't be decoded as an
// image at all. Static and stateless (touches only its own arguments) —
// deliberately safe to call from a worker thread; see its own header
// comment for why that matters.
Glib::RefPtr<Gdk::Texture> ArtCache::DecodeScaledTexture(const Glib::RefPtr<Glib::Bytes>& raw_bytes, int target_size)
{
  try
  {
    auto stream = Gio::MemoryInputStream::create();
    gsize size = 0;
    gconstpointer data = raw_bytes->get_data(size);
    stream->add_data(data, static_cast<gssize>(size), nullptr);
    auto pixbuf = Gdk::Pixbuf::create_from_stream_at_scale(stream, target_size, target_size, true);
    return Gdk::Texture::create_for_pixbuf(pixbuf);
  }
  catch (const Glib::Error&)
  {
    return {};
  }
}

ArtCache& ArtCache::Instance()
{
  static ArtCache instance;
  return instance;
}

ArtCache::ArtCache()
{
  LoadSettings();
}

std::string ArtCache::CacheDir()
{
  return Glib::build_filename(Glib::get_user_cache_dir(), "gnomos", "art-cache");
}

std::string ArtCache::PathFor(const std::string& uri) const
{
  // SHA-256 of the URI as the filename: URIs contain characters that
  // aren't safe as-is (":", "/", "?", ...), and this also naturally caps
  // the filename length regardless of how long a real art_uri gets.
  return Glib::build_filename(CacheDir(), Glib::Checksum::compute_checksum(Glib::Checksum::Type::SHA256, uri));
}

void ArtCache::LoadSettings()
{
  auto keyfile = Glib::KeyFile::create();
  try
  {
    if (keyfile->load_from_file(SettingsPath()))
      max_disk_mb_ = static_cast<unsigned>(keyfile->get_integer("cache", "max_mb"));
  }
  catch (const Glib::Error&)
  {
    // fine — first launch, or the key/file doesn't exist yet; keep the default
  }
}

void ArtCache::SaveSettings() const
{
  const std::string dir = Glib::build_filename(Glib::get_user_config_dir(), "gnomos");
  g_mkdir_with_parents(dir.c_str(), 0700);

  auto keyfile = Glib::KeyFile::create();
  try
  {
    keyfile->load_from_file(SettingsPath());
  }
  catch (const Glib::Error&)
  {
    // fine — first time this file is written
  }
  keyfile->set_integer("cache", "max_mb", static_cast<int>(max_disk_mb_));
  try
  {
    keyfile->save_to_file(SettingsPath());
  }
  catch (const Glib::Error&)
  {
    // non-fatal — the limit just won't be remembered next launch
  }
}

void ArtCache::SetMaxDiskMb(unsigned mb)
{
  max_disk_mb_ = mb;
  SaveSettings();
  EnforceDiskLimit();
}

void ArtCache::Touch(const std::string& uri)
{
  auto it = entries_.find(uri);
  if (it == entries_.end())
    return;
  lru_.erase(it->second.lru_it);
  lru_.push_front(uri);
  it->second.lru_it = lru_.begin();
}

void ArtCache::InsertMemory(const std::string& uri, const Glib::RefPtr<Gdk::Texture>& texture,
                             const Glib::RefPtr<Glib::Bytes>& raw_bytes)
{
  auto it = entries_.find(uri);
  if (it != entries_.end())
  {
    it->second.texture = texture;
    if (raw_bytes)
      it->second.raw_bytes = raw_bytes;
    Touch(uri);
    return;
  }

  if (entries_.size() >= kMaxMemoryEntries)
  {
    const std::string& victim = lru_.back();
    entries_.erase(victim);
    lru_.pop_back();
  }

  lru_.push_front(uri);
  entries_.emplace(uri, Entry{texture, raw_bytes, lru_.begin()});
}

Glib::RefPtr<Gdk::Texture> ArtCache::Get(const std::string& uri)
{
  auto it = entries_.find(uri);
  if (it != entries_.end())
  {
    Touch(uri);
    // A memory entry can have an empty texture with real raw_bytes —
    // GetRawBytes()'s own disk-fallback deliberately skips the full-
    // resolution decode (see its own comment), promising this exact
    // lazy decode-on-first-actual-need in exchange. Confirmed live: this
    // was never actually implemented, only promised in that comment —
    // PlayerBar::LoadArt() (the only caller that still wants a full-res
    // texture rather than GetScaled()'s own scaled one) got back a
    // silently-empty RefPtr for anything that had gone through that
    // disk-fallback path first, showing its own fallback icon forever
    // even though a real image was one decode away the whole time.
    if (!it->second.texture && it->second.raw_bytes)
    {
      try
      {
        it->second.texture = Gdk::Texture::create_from_bytes(it->second.raw_bytes);
      }
      catch (const Glib::Error&)
      {
        // raw_bytes turned out not to be decodable after all — leave
        // texture empty, same as any other "no art available" case.
      }
    }
    return it->second.texture;
  }

  // Not in memory (e.g. fresh app start) — fall back to the disk cache
  // before giving up and making the caller do a fresh HTTP fetch.
  const std::string path = PathFor(uri);
  if (!Glib::file_test(path, Glib::FileTest::EXISTS))
    return {};

  try
  {
    std::string contents = Glib::file_get_contents(path);
    auto bytes = Glib::Bytes::create(contents.data(), contents.size());
    auto texture = Gdk::Texture::create_from_bytes(bytes);
    InsertMemory(uri, texture, bytes);
    // Re-touch the file's mtime so this counts as recently used for the
    // mtime-based eviction order in EnforceDiskLimit() — otherwise a
    // frequently-viewed-but-never-re-Put() art would look stale and get
    // evicted first despite being the most relevant one to keep.
    auto file = Gio::File::create_for_path(path);
    auto now = Glib::DateTime::create_now_local();
    file->set_attribute_uint64(G_FILE_ATTRIBUTE_TIME_MODIFIED, static_cast<guint64>(now.to_unix()),
                                Gio::FileQueryInfoFlags::NONE);
    return texture;
  }
  catch (const Glib::Error&)
  {
    return {};
  }
}

Glib::RefPtr<Gdk::Texture> ArtCache::GetScaled(const std::string& uri, int target_size)
{
  auto bytes = GetRawBytes(uri);
  if (!bytes)
    return {};
  return DecodeScaledTexture(bytes, target_size);
}

Glib::RefPtr<Glib::Bytes> ArtCache::GetRawBytes(const std::string& uri)
{
  auto it = entries_.find(uri);
  if (it != entries_.end() && it->second.raw_bytes)
  {
    Touch(uri);
    return it->second.raw_bytes;
  }

  // Not in memory (or a memory entry exists but predates raw_bytes being
  // tracked — can't happen post-restart, but harmless to also just fall
  // through to disk here) — same disk fallback Get() has.
  const std::string path = PathFor(uri);
  if (!Glib::file_test(path, Glib::FileTest::EXISTS))
    return {};

  try
  {
    std::string contents = Glib::file_get_contents(path);
    auto bytes = Glib::Bytes::create(contents.data(), contents.size());
    // No eager full-resolution decode here on purpose — confirmed live,
    // this was the other half (alongside the scaled decode ArtDecodePool
    // now handles) of what made rebuilding a large grid take seconds: most
    // of a 1000+-entry grid's tiles land in this disk-fallback branch
    // (kMaxMemoryEntries is far smaller than that), and every one of them
    // used to pay for a full-resolution Gdk::Texture::create_from_bytes()
    // synchronously right here, even though a grid tile only ever wants
    // the scaled version. InsertMemory() with an empty texture just means
    // "raw bytes cached, full-res texture not decoded yet" — Get()'s own
    // memory-hit branch below decodes it lazily, on demand, the one time
    // something (PlayerBar) actually asks for the full-resolution image.
    InsertMemory(uri, {}, bytes);
    auto file = Gio::File::create_for_path(path);
    auto now = Glib::DateTime::create_now_local();
    file->set_attribute_uint64(G_FILE_ATTRIBUTE_TIME_MODIFIED, static_cast<guint64>(now.to_unix()),
                                Gio::FileQueryInfoFlags::NONE);
    return bytes;
  }
  catch (const Glib::Error&)
  {
    return {};
  }
}

Glib::RefPtr<Gdk::Texture> ArtCache::Put(const std::string& uri, const Glib::RefPtr<Glib::Bytes>& raw_bytes)
{
  Glib::RefPtr<Gdk::Texture> texture;
  try
  {
    texture = Gdk::Texture::create_from_bytes(raw_bytes);
  }
  catch (const Glib::Error&)
  {
    return {};
  }

  InsertMemory(uri, texture, raw_bytes);

  // Best-effort disk write — a failure here (disk full, permissions, ...)
  // just means this particular art won't survive a restart, not a reason
  // to fail the whole operation; the caller already has a perfectly good
  // decoded texture either way.
  try
  {
    g_mkdir_with_parents(CacheDir().c_str(), 0700);
    gsize size = 0;
    gconstpointer data = raw_bytes->get_data(size);
    Glib::file_set_contents(PathFor(uri), static_cast<const gchar*>(data), static_cast<gssize>(size));
    EnforceDiskLimit();
  }
  catch (const Glib::Error&)
  {
  }

  return texture;
}

void ArtCache::EnforceDiskLimit()
{
  const std::uint64_t budget = static_cast<std::uint64_t>(max_disk_mb_) * 1024 * 1024;

  auto dir = Gio::File::create_for_path(CacheDir());
  Glib::RefPtr<Gio::FileEnumerator> enumerator;
  try
  {
    enumerator = dir->enumerate_children(G_FILE_ATTRIBUTE_STANDARD_NAME "," G_FILE_ATTRIBUTE_STANDARD_SIZE ","
                                          G_FILE_ATTRIBUTE_TIME_MODIFIED);
  }
  catch (const Glib::Error&)
  {
    return;  // cache dir doesn't exist yet, or genuinely unreadable — nothing to enforce
  }

  struct FileStat
  {
    std::string name;
    std::uint64_t size;
    Glib::DateTime mtime;
  };
  std::vector<FileStat> files;
  std::uint64_t total = 0;
  Glib::RefPtr<Gio::FileInfo> info;
  while ((info = enumerator->next_file()))
  {
    std::uint64_t size = static_cast<std::uint64_t>(info->get_size());
    files.push_back({info->get_name(), size, info->get_modification_date_time()});
    total += size;
  }
  if (total <= budget)
    return;

  // Oldest-modified first — a cache hit re-touches its file's mtime (see
  // Get()'s disk-fallback path in the header comment), so this is a
  // reasonable least-recently-used approximation without needing to
  // maintain a separate index that would itself have to survive restarts.
  std::sort(files.begin(), files.end(),
            [](const FileStat& a, const FileStat& b) { return a.mtime.to_unix() < b.mtime.to_unix(); });

  for (const FileStat& file : files)
  {
    if (total <= budget)
      break;
    auto child = Gio::File::create_for_path(Glib::build_filename(CacheDir(), file.name));
    try
    {
      child->remove();
      total -= file.size;
    }
    catch (const Glib::Error&)
    {
      // leave it — will be retried on the next EnforceDiskLimit() call
    }
  }
}

void ArtCache::Clear()
{
  entries_.clear();
  lru_.clear();

  auto dir = Gio::File::create_for_path(CacheDir());
  Glib::RefPtr<Gio::FileEnumerator> enumerator;
  try
  {
    enumerator = dir->enumerate_children(G_FILE_ATTRIBUTE_STANDARD_NAME);
  }
  catch (const Glib::Error&)
  {
    return;  // nothing on disk yet
  }

  Glib::RefPtr<Gio::FileInfo> info;
  while ((info = enumerator->next_file()))
  {
    auto child = Gio::File::create_for_path(Glib::build_filename(CacheDir(), info->get_name()));
    try
    {
      child->remove();
    }
    catch (const Glib::Error&)
    {
    }
  }
}

std::uint64_t ArtCache::GetDiskUsageBytes() const
{
  auto dir = Gio::File::create_for_path(CacheDir());
  Glib::RefPtr<Gio::FileEnumerator> enumerator;
  try
  {
    enumerator = dir->enumerate_children(G_FILE_ATTRIBUTE_STANDARD_SIZE);
  }
  catch (const Glib::Error&)
  {
    return 0;
  }

  std::uint64_t total = 0;
  Glib::RefPtr<Gio::FileInfo> info;
  while ((info = enumerator->next_file()))
    total += static_cast<std::uint64_t>(info->get_size());
  return total;
}

}  // namespace gnomos
