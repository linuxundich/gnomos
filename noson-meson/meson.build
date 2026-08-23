# Meson build for libnoson (https://github.com/janbar/noson, upstream
# CMake-only), vendored as a git submodule under noson/ — see its own
# .gitmodules entry. Deliberately kept *outside* that submodule directory
# (rather than living at noson/noson/meson.build, as it used to when noson/
# was a plain vendored copy) so the submodule stays a pristine, unmodified
# checkout of upstream; every path below is written relative to this file's
# own location instead.
#
# This mirrors noson/noson/CMakeLists.txt and is included via subdir() from
# the top-level meson.build — it intentionally has no project() call of its
# own, it just adds targets to the parent project. libnoson's optional
# local-audio streaming feature set (FLAC/PulseAudio source,
# src/flacencoder.*, src/pa*.cpp, src/pulsestreamer.*) is not built: a
# remote-control client never needs to originate audio itself.

cc = meson.get_compiler('c')

noson_threads_dep = dependency('threads')
noson_zlib_dep = dependency('zlib')
noson_openssl_dep = dependency('openssl')
noson_dl_dep = cc.find_library('dl', required: false)
noson_m_dep = cc.find_library('m', required: false)
noson_rt_dep = cc.find_library('rt', required: false)

# noson/src/*.h include "local_config.h" unqualified; CMake generates it via
# configure_file from src/local_config.h.in into a build-tree "public" dir.
# format: 'cmake@' handles both the "#cmakedefine FOO @FOO@" lines (optional
# features) and the plain "#define FOO @FOO@" substitutions in that file.
noson_cdata = configuration_data()
noson_cdata.set('HAVE_TIMEGM', cc.has_function('timegm') ? 1 : 0)
noson_cdata.set('HAVE_LOCALTIME_R', cc.has_function('localtime_r') ? 1 : 0)
noson_cdata.set('HAVE_GMTIME_R', cc.has_function('gmtime_r') ? 1 : 0)
noson_cdata.set('HAVE_ZLIB', 1)
noson_cdata.set('HAVE_OPENSSL', 1)
noson_cdata.set('CC_INLINE', 'inline')
noson_cdata.set('NOSON_LIB_VERSION', '2.13.2')
noson_cdata.set('noson_VERSION_MAJOR', 2)
noson_cdata.set('noson_VERSION_MINOR', 13)
noson_cdata.set('noson_VERSION_PATCH', 2)
# HAVE_PULSEAUDIO / HAVE_FLAC / HAVE_MP3LAME left unset on purpose (see above).

noson_local_config_h = configure_file(
  input: '../noson/noson/src/local_config.h.in',
  output: 'local_config.h',
  format: 'cmake@',
  configuration: noson_cdata,
)

# include_directories() always adds both the source-tree and build-tree
# variant of each path, so '.' (this directory) also picks up the generated
# local_config.h that configure_file() just placed in the build tree.
noson_inc = include_directories('../noson/noson/src', '.')

noson_src = '../noson/noson/'

noson_sources = files(
  # src/private/*.c
  noson_src + 'src/private/builtin.c',
  noson_src + 'src/private/wsstatic.c',
  # src/private/*.cpp
  noson_src + 'src/private/base64.cpp',
  noson_src + 'src/private/byteorder.cpp',
  noson_src + 'src/private/compressor.cpp',
  noson_src + 'src/private/debug.cpp',
  noson_src + 'src/private/eventbroker.cpp',
  noson_src + 'src/private/jsonparser.cpp',
  noson_src + 'src/private/mainpagebroker.cpp',
  noson_src + 'src/private/pcmblankkiller.cpp',
  noson_src + 'src/private/ringbuffer.cpp',
  noson_src + 'src/private/securesocket.cpp',
  noson_src + 'src/private/socket.cpp',
  noson_src + 'src/private/tinyxml2.cpp',
  noson_src + 'src/private/upnpnotificationbroker.cpp',
  noson_src + 'src/private/uriencoder.cpp',
  noson_src + 'src/private/uriparser.cpp',
  noson_src + 'src/private/wsreplychunked.cpp',
  noson_src + 'src/private/wsrequestbroker.cpp',
  noson_src + 'src/private/wsrequest.cpp',
  noson_src + 'src/private/wsrequestreply.cpp',
  noson_src + 'src/private/wsresponse.cpp',
  noson_src + 'src/private/xmldict.cpp',
  # OS threading primitives (src/private/os/threads)
  noson_src + 'src/private/os/threads/threadpool.cpp',
  noson_src + 'src/private/os/threads/latch.cpp',
  # public library sources
  noson_src + 'src/data/datareader.cpp',
  noson_src + 'src/alarmclock.cpp',
  noson_src + 'src/alarm.cpp',
  noson_src + 'src/avtransport.cpp',
  noson_src + 'src/contentdirectory.cpp',
  noson_src + 'src/deviceproperties.cpp',
  noson_src + 'src/didlparser.cpp',
  noson_src + 'src/digitalitem.cpp',
  noson_src + 'src/eventhandler.cpp',
  noson_src + 'src/filepicreader.cpp',
  noson_src + 'src/filestreamer.cpp',
  noson_src + 'src/imageservice.cpp',
  noson_src + 'src/intrinsic.cpp',
  noson_src + 'src/iostream.cpp',
  noson_src + 'src/locked.cpp',
  noson_src + 'src/musicservices.cpp',
  noson_src + 'src/renderingcontrol.cpp',
  noson_src + 'src/requestbroker.cpp',
  noson_src + 'src/service.cpp',
  noson_src + 'src/sharedptr.cpp',
  noson_src + 'src/smaccount.cpp',
  noson_src + 'src/smapi.cpp',
  noson_src + 'src/smapimetadata.cpp',
  noson_src + 'src/sonosplayer.cpp',
  noson_src + 'src/sonossystem.cpp',
  noson_src + 'src/sonostypes.cpp',
  noson_src + 'src/sonoszone.cpp',
  noson_src + 'src/subscription.cpp',
  noson_src + 'src/subscriptionpool.cpp',
  noson_src + 'src/zonegrouptopology.cpp',
)

noson_lib = static_library(
  'noson',
  noson_sources, noson_local_config_h,
  include_directories: noson_inc,
  dependencies: [
    noson_threads_dep, noson_zlib_dep, noson_openssl_dep,
    noson_dl_dep, noson_m_dep, noson_rt_dep,
  ],
  install: false,
)

noson_dep = declare_dependency(
  link_with: noson_lib,
  include_directories: noson_inc,
  dependencies: [noson_threads_dep],
)
