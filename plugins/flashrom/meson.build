if get_option('plugin_flashrom')
cargs = ['-DG_LOG_DOMAIN="FuPluginFlashrom"']

install_data(['flashrom.quirk'],
  install_dir: join_paths(get_option('datadir'), 'fwupd', 'quirks.d')
)

shared_module('fu_plugin_flashrom',
  fu_hash,
  sources : [
    'fu-flashrom-device.c',
    'fu-flashrom-internal-device.c',
    'fu-plugin-flashrom.c',
  ],
  include_directories : [
    root_incdir,
    fwupd_incdir,
    fwupdplugin_incdir,
  ],
  install : true,
  install_dir: plugin_dir,
  link_with : [
    fwupd,
    fwupdplugin,
  ],
  c_args : [
    cargs,
    '-DLOCALSTATEDIR="' + localstatedir + '"',
  ],
  dependencies : [
    plugin_deps,
    libflashrom,
  ],
)
endif
