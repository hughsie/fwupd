if get_option('plugin_parade_lspcon')

if not get_option('gudev')
  error('gudev is required for plugin_parade_lspcon')
endif

cargs = ['-DG_LOG_DOMAIN="FuPluginParadeLspcon"']

install_data(['parade-lspcon.quirk'],
  install_dir: join_paths(get_option('datadir'), 'fwupd', 'quirks.d')
)

shared_module('fu_plugin_parade_lspcon',
  fu_hash,
  sources : [
    'fu-parade-lspcon-device.c',
    'fu-plugin-parade-lspcon.c',
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
  ],
)
endif
