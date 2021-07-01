if get_option('plugin_kinetic_dp')
if not get_option('gudev')
  error('gudev is required for plugin_kinetic_dp')
endif
cargs = ['-DG_LOG_DOMAIN="FuPluginKineticDp"']

install_data(['kinetic-dp.quirk'],
  install_dir: join_paths(datadir, 'fwupd', 'quirks.d')
)

shared_module('fu_plugin_kinetic_dp',
  fu_hash,
  sources : [
    'fu-plugin-kinetic-dp.c',
    'fu-kinetic-dp-common.c',
    'fu-kinetic-dp-connection.c',
    'fu-kinetic-dp-device.c',
    'fu-kinetic-dp-firmware.c',
    'fu-kinetic-dp-aux-dpcd.c',
    'fu-kinetic-dp-aux-isp.c',
    'fu-kinetic-dp-secure-aux-isp.c',
  ],
  include_directories : [
    root_incdir,
    fwupd_incdir,
    fwupdplugin_incdir,
  ],
  install : true,
  install_dir: plugin_dir,
  c_args : [
    cargs,
  ],
  link_with : [
    fwupd,
    fwupdplugin,
  ],
  dependencies : [
    plugin_deps,
  ],
)
endif
