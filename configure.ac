
AC_PREREQ([2.61])
AC_INIT([ezwebsocket], [1.0], [clemens.kresser@gmail.com])
AC_CONFIG_AUX_DIR(aux-dist)
AM_INIT_AUTOMAKE

AC_PROG_MAKE_SET
AC_PROG_CC
AC_LANG(C)

CFLAGS="-std=gnu99 -fPIC -Wall -Wextra -Wpedantic"

AC_CONFIG_MACRO_DIR([m4])

LT_PREREQ([2.2])
LT_INIT([dlopen shared])

AC_CONFIG_FILES([Makefile inc/Makefile src/Makefile])

# Checks for header files.
AC_CHECK_HEADERS([stdbool.h \
		stdlib.h \
		stddef.h \
		string.h \
		ctype.h \
		time.h \
		sys/socket.h \
		arpa/inet.h \
		sys/select.h \
		errno.h \
		pthread.h \
		unistd.h \
        stdint.h \
		])

# Checks for typedefs
AC_TYPE_SIZE_T

#Check for libs
AC_CHECK_LIB(pthread, pthread_create, [LIBS="-lpthread ${LIBS}"], [
                                                echo "pthreads required, failing"
                                                exit -1
                                                ])

AC_OUTPUT

