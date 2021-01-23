lib_LTLIBRARIES = libezwebsocket.la
libezwebsocket_la_SOURCES = ./websocket.c \
                            ./utils/base64.c \
                            ./utils/dyn_buffer.c \
                            ./utils/log.c \
                            ./utils/ref_count.c \
                            ./utils/sha1.c \
                            ./utils/stringck.c \
                            ./utils/utf8.c \
                            ./socket_server/socket_server.c \
                            ./socket_client/socket_client.c
libezwebsocket_la_LIBADD = -lpthread
AUTOMAKE_OPTIONS = subdir-objects
AM_CFLAGS = -I$(srcdir) -I$(top_srcdir)/inc 
#-I$(includedir)
