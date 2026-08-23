gnomos_sources = files(
  'main.cpp',
  'gnomos-application.cpp',
  'gnomos-window.cpp',
  'mpris-service.cpp',
  'backend/noson-backend.cpp',
  'widgets/alarms-view.cpp',
  'widgets/art-cache.cpp',
  'widgets/art-decode-pool.cpp',
  'widgets/artist-image-fetcher.cpp',
  'widgets/cover-thumbnail.cpp',
  'widgets/favorites-view.cpp',
  'widgets/history-view.cpp',
  'widgets/library-view.cpp',
  'widgets/player-bar.cpp',
  'widgets/queue-view.cpp',
)

executable(
  'gnomos',
  gnomos_sources,
  config_h,
  include_directories: config_inc,
  dependencies: [gtkmm_dep, adwaita_dep, noson_dep, json_glib_dep],
  install: true,
)
