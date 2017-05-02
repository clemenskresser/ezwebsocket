#!/bin/bash
rm -R autom4te.cache
rm -R aux-dist
rm -R m4
rm aclocal.m4 config.log config.status configure libtool Makefile Makefile.in
rm src/*.o src/*.la src/*.lo src/Makefile src/Makefile.in
rm src/utils/*.o src/utils/*.lo
rm src/socket_server/*.o src/socket_server/*.lo
rm inc/Makefile inc/Makefile.in
rm INSTALL
rm -R src/.deps
rm -R src/.libs
rm src/.dirstamp
rm -R src/utils/.deps
rm -R src/utils/.libs
rm -R src/utils/.dirstamp
rm -R src/socket_server/.deps
rm -R src/socket_server/.libs
rm -R src/socket_server/.dirstamp

echo done

