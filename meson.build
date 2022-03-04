project(
  'buttond',
  'c',
  version: '0.1',
  license: 'MIT',
  default_options: [
    'c_std=gnu17',
    'warning_level=2',
    'werror=true',
  ],
)

cc = meson.get_compiler('c')
add_project_arguments(cc.get_supported_arguments([
  '-D_POSIX_C_SOURCE=200809L',
]), language: 'c')

executable(
  'buttond',
  'buttond.c', 'input.c',
  install: true
)

install_data(
  'openrc/init.d/buttond',
  install_dir: '/etc/init.d'
)
install_data(
  'openrc/conf.d/buttond',
  install_dir: '/etc/conf.d'
)

test('all tests', find_program('./tests.sh'))
