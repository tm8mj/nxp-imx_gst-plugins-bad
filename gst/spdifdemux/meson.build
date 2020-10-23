gstspdifdemux = library('gstspdifdemux',
  'gstspdifdemux.c',
  c_args : gst_plugins_bad_args,
  include_directories : [configinc, libsinc],
  dependencies : [gstbase_dep, gstaudio_dep, libm],
  install : true,
  install_dir : plugins_install_dir,
)
pkgconfig.generate(gstspdifdemux, install_dir : plugins_pkgconfig_install_dir)
