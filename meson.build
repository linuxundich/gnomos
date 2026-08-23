project(
  'gnomos',
  ['c', 'cpp'],
  version: '0.3.6',
  license: 'GPL-3.0-or-later',
  default_options: [
    'cpp_std=c++17',
    'warning_level=2',
  ],
  meson_version: '>= 0.60.0',
)

gnome = import('gnome')

application_id = 'de.christophlangner.Gnomos'

conf = configuration_data()
conf.set_quoted('PACKAGE_NAME', meson.project_name())
conf.set_quoted('PACKAGE_VERSION', meson.project_version())
conf.set_quoted('APPLICATION_ID', application_id)
conf.set_quoted('SOURCE_ROOT', meson.project_source_root())
config_h = configure_file(output: 'config.h', configuration: conf)
# add_project_arguments() must run before any build target is declared
# (i.e. before subdir('noson-meson'), which defines the noson static_library).
add_project_arguments('-DHAVE_CONFIG_H', language: ['c', 'cpp'])
config_inc = include_directories('.')

# --- libnoson (git submodule under noson/, upstream ships only a CMake
# build) — the meson.build for it deliberately lives in noson-meson/, not
# inside the submodule itself, so noson/ stays a pristine, unmodified
# checkout of upstream. See noson-meson/meson.build's own header comment.
subdir('noson-meson')

# --- GTK4 / libadwaita / gtkmm4 stack ---
gtkmm_dep = dependency('gtkmm-4.0', version: '>= 4.10')
adwaita_dep = dependency('libadwaita-1', version: '>= 1.4')
# Only for ArtistImageFetcher's own Deezer JSON responses (see its header
# comment) — small, predictable-shape API responses, not worth hand-rolling
# a parser for the way NosonBackend does for a handful of single XML tags
# elsewhere.
json_glib_dep = dependency('json-glib-1.0')

subdir('data')
subdir('src')

gnome.post_install(
  gtk_update_icon_cache: true,
  update_desktop_database: true,
)
