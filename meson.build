project(
  'buttond',
  'c',
  version: run_command(
    ['sh', '-c',
      'git describe 2>/dev/null || awk -F"\"" "/define BUTTOND_VERSION/ { print \$2 }" version.h'
    ], check: true).stdout().strip(),
  license: 'MIT',
  default_options: [
    'c_std=gnu17',
    'warning_level=2',
    'werror=true',
  ],
)

cc = meson.get_compiler('c')
add_project_arguments(cc.get_supported_arguments([
  '-D_GNU_SOURCE',
  '-DBUTTOND_VERSION="' + meson.project_version() + '"',
]), language: 'c')

executable(
  'buttond',
  'buttond.c', 'input.c', 'keys.c',
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
