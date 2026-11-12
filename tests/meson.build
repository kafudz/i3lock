test_util = executable(
  'test_util',
  'test_util.c',
  '../util.c',
  include_directories: inc,
  dependencies: [config_h],
)
test('util', test_util)

test_password = executable(
  'test_password',
  'test_password.c',
  '../password.c',
  '../util.c',
  include_directories: inc,
  dependencies: [config_h],
)
test('password', test_password)

test_image = executable(
  'test_image',
  'test_image.c',
  '../image.c',
  '../util.c',
  include_directories: inc,
  dependencies: [config_h, cairo_dep],
)
test('image', test_image)
