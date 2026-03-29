project('satwave', 'c',
  version: '0.1.0',
  meson_version: '>= 1.0.0',
  default_options: [
    'warning_level=2',
    'c_std=gnu11',
  ],
)

gnome = import('gnome')

# Dependencies
gtk_dep       = dependency('gtk4', version: '>= 4.14')
adw_dep       = dependency('libadwaita-1', version: '>= 1.4')
gst_dep       = dependency('gstreamer-1.0', version: '>= 1.22')
gst_base_dep  = dependency('gstreamer-base-1.0', version: '>= 1.22')
soup_dep      = dependency('libsoup-3.0', version: '>= 3.4')
json_dep      = dependency('json-glib-1.0', version: '>= 1.6')
secret_dep    = dependency('libsecret-1', version: '>= 0.20')
pw_dep        = dependency('libpipewire-0.3', version: '>= 0.3')

conf_data = configuration_data()
conf_data.set_quoted('APP_ID', 'com.github.satwave')
conf_data.set_quoted('APP_VERSION', meson.project_version())
conf_data.set_quoted('GETTEXT_PACKAGE', meson.project_name())

config_h = configure_file(
  output: 'satwave-config.h',
  configuration: conf_data,
)

config_inc = include_directories('.')

# Compile GResources
resources = gnome.compile_resources(
  'satwave-resources',
  'data/satwave.gresource.xml',
  source_dir: 'data',
  c_name: 'satwave',
)

# GSettings schema
install_data(
  'data/com.github.satwave.gschema.xml',
  install_dir: get_option('datadir') / 'glib-2.0' / 'schemas',
)

gnome.compile_schemas(
  depend_files: files('data/com.github.satwave.gschema.xml'),
)

sources = files(
  'src/main.c',
  'src/satwave-application.c',
  'src/satwave-window.c',
  'src/satwave-player.c',
  'src/satwave-api-client.c',
  'src/satwave-auth.c',
  'src/satwave-hls-proxy.c',
  'src/satwave-channel.c',
  'src/satwave-channel-store.c',
  'src/satwave-login-dialog.c',
  'src/satwave-mpris.c',
  'src/satwave-call-monitor.c',
)

executable('satwave',
  sources + resources,
  config_h,
  include_directories: config_inc,
  dependencies: [
    gtk_dep,
    adw_dep,
    gst_dep,
    gst_base_dep,
    soup_dep,
    json_dep,
    secret_dep,
    pw_dep,
  ],
  install: true,
)
