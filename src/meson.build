
srcs_websocket = [
  'utils/base64.c',
  'utils/dyn_buffer.c',
  'utils/log.c',
  'utils/ref_count.c',
  'utils/stringck.c',
  'utils/utf8.c',
  'utils/sha1.c',
  'socket_client/socket_client.c',
  'socket_server/socket_server.c',
  'websocket.c',
]

inc_websocket = [
  inc_common,
  include_directories('utils'),
  include_directories('socket_client'),
  include_directories('socket_server'),
]

deps_websocket = [
  dep_threads,
]

libezwebsocket = shared_library(
    'ezwebsocket',
    srcs_websocket,
    include_directories : inc_websocket,
    version : meson.project_version(),
    dependencies: deps_websocket,
    install : true)

dep_libezwebsocket = declare_dependency(
    link_with: libezwebsocket,
    include_directories : inc_websocket,
    )

pkg.generate(libezwebsocket)

