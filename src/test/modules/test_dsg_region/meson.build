# 真实 mmap Provider 只在支持的 Unix 开发/运行平台执行。
if host_system == 'windows'
  subdir_done()
endif

test_dsg_region = shared_module('test_dsg_region',
  files('test_dsg_region.c'),
  kwargs: pg_test_mod_args,
)
test_install_libs += test_dsg_region
test_install_data += files('test_dsg_region.control', 'test_dsg_region--1.0.sql')

tests += {
  'name': 'test_dsg_region',
  'sd': meson.current_source_dir(),
  'bd': meson.current_build_dir(),
  'regress': {
    'sql': ['test_dsg_region'],
    'regress_args': ['--temp-config', files('test_dsg_region.conf')],
    'runningcheck': false,
  },
  'isolation': {
    'specs': ['test_dsg_region_shared'],
    'regress_args': ['--temp-config', files('test_dsg_region.conf')],
    'runningcheck': false,
  },
}
