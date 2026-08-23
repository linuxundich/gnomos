# Meson build for the vendored libnoson sources
# (upstream: https://github.com/janbar/libnoson, CMake-only).
#
# This mirrors noson/CMakeLists.txt in this directory and is included via
# subdir() from the top-level meson.build — it intentionally has no
# project() call of its own, it just adds targets to the parent project.
# libnoson's optional local-audio streaming feature set (FLAC/PulseAudio
# source, src/flacencoder.*, src/pa*.cpp, src/pulsestreamer.*) is not built:
# a remote-control client never needs to originate audio itself.

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
  input: 'src/local_config.h.in',
  output: 'local_config.h',
  format: 'cmake@',
  configuration: noson_cdata,
)

# include_directories() always adds both the source-tree and build-tree
# variant of each path, so '.' (this directory) also picks up the generated
# local_config.h that configure_file() just placed in the build tree.
noson_inc = include_directories('src', '.')

noson_sources = files(
  # src/private/*.c
  'src/private/builtin.c',
  'src/private/wsstatic.c',
  # src/private/*.cpp
  'src/private/base64.cpp',
  'src/private/byteorder.cpp',
  'src/private/compressor.cpp',
  'src/private/debug.cpp',
  'src/private/eventbroker.cpp',
  'src/private/jsonparser.cpp',
  'src/private/mainpagebroker.cpp',
  'src/private/pcmblankkiller.cpp',
  'src/private/ringbuffer.cpp',
  'src/private/securesocket.cpp',
  'src/private/socket.cpp',
  'src/private/tinyxml2.cpp',
  'src/private/upnpnotificationbroker.cpp',
  'src/private/uriencoder.cpp',
  'src/private/uriparser.cpp',
  'src/private/wsreplychunked.cpp',
  'src/private/wsrequestbroker.cpp',
  'src/private/wsrequest.cpp',
  'src/private/wsrequestreply.cpp',
  'src/private/wsresponse.cpp',
  'src/private/xmldict.cpp',
  # OS threading primitives (src/private/os/threads)
  'src/private/os/threads/threadpool.cpp',
  'src/private/os/threads/latch.cpp',
  # public library sources
  'src/data/datareader.cpp',
  'src/alarmclock.cpp',
  'src/alarm.cpp',
  'src/avtransport.cpp',
  'src/contentdirectory.cpp',
  'src/deviceproperties.cpp',
  'src/didlparser.cpp',
  'src/digitalitem.cpp',
  'src/eventhandler.cpp',
  'src/filepicreader.cpp',
  'src/filestreamer.cpp',
  'src/imageservice.cpp',
  'src/intrinsic.cpp',
  'src/iostream.cpp',
  'src/locked.cpp',
  'src/musicservices.cpp',
  'src/renderingcontrol.cpp',
  'src/requestbroker.cpp',
  'src/service.cpp',
  'src/sharedptr.cpp',
  'src/smaccount.cpp',
  'src/smapi.cpp',
  'src/smapimetadata.cpp',
  'src/sonosplayer.cpp',
  'src/sonossystem.cpp',
  'src/sonostypes.cpp',
  'src/sonoszone.cpp',
  'src/subscription.cpp',
  'src/subscriptionpool.cpp',
  'src/zonegrouptopology.cpp',
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
