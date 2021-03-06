#
# Host Identity Protocol
# Copyright (c) 2006 the Boeing Comapny
#
# This program is free software; you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation; either version 2 of the License, or
# (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# configure.ac
#
# Author: Jeff Ahrenholz <jeffrey.m.ahrenholz@boeing.com>
#
#

#                                               -*- Autoconf -*-
# Process this file with autoconf to produce a configure script.

AC_PREREQ(2.59)
# change the HIP version here
AC_INIT(OpenHIP, 0.6, [openhip-developers@lists.sourceforge.net], openhip)
AC_CONFIG_SRCDIR([src/protocol/hip_main.c])
AC_CONFIG_AUX_DIR(config)
AM_CONFIG_HEADER([config.h])
AM_INIT_AUTOMAKE

# Default compiler flags, to avoid using -g with release files.
AC_GNU_SOURCE
CFLAGS="$CFLAGS -O3 -Werror -D_GNU_SOURCE"

# HIP files go in /etc/hip or /usr/local/etc/hip
sysconfdir=$sysconfdir/hip

# Checks for programs.
AC_PROG_AWK
AC_PROG_CC
AM_PROG_CC_C_O

# Checks for libraries.
AC_CHECK_LIB([crypto], [AES_cbc_encrypt])
if test "x$ac_cv_lib_crypto_AES_cbc_encrypt" != "xyes"; then
	AC_MSG_ERROR([Could not find AES functions from OpenSSL. Make sure the openssl-devel package is installed or that you have installed the suitable source.])
fi
AC_CHECK_LIB([crypto], [RSA_verify])
if test "x$ac_cv_lib_crypto_RSA_verify" != "xyes"; then
	AC_MSG_ERROR([Could not find RSA functions from OpenSSL. Make sure the openssl-devel package is installed or that you have installed the suitable source.])
fi

#
# configure option to define SMA_CRAWLER
AC_CHECK_LIB([pthread], [pthread_create])
# FIXME: Replace `main' with a function in `-li3', `-li3client':
#AC_CHECK_LIB([i3], [main])
#AC_CHECK_LIB([i3client], [main])
# FIXME: Replace `main' with a function in `-liphlpapi':
#AC_CHECK_LIB([iphlpapi], [])
AC_CHECK_LIB([ipsec], [pfkey_send_hip_x1])
# FIXME: Replace `main' with a function in `-lutils':
#AC_CHECK_LIB([utils], [main])

# Checks for header files.
AC_HEADER_STDC
AC_HEADER_SYS_WAIT
AC_CHECK_HEADERS([arpa/inet.h fcntl.h netdb.h netinet/in.h stdlib.h string.h sys/ioctl.h sys/socket.h sys/time.h unistd.h])

# Checks for typedefs, structures, and compiler characteristics.
AC_C_CONST
AC_C_INLINE
AC_TYPE_SIZE_T
AC_HEADER_TIME

# Checks for library functions.
AC_FUNC_FORK
AC_PROG_GCC_TRADITIONAL
AC_FUNC_MALLOC
AC_FUNC_MEMCMP
AC_FUNC_SELECT_ARGTYPES
AC_TYPE_SIGNAL
AC_FUNC_VPRINTF
AC_CHECK_FUNCS([bzero gethostbyname gethostname gettimeofday inet_ntoa memmove memset pow select socket strchr strerror strncasecmp strrchr strstr uname])

CFLAGS="$CFLAGS -Wall -DCONFIG_HIP -DSYSCONFDIR=\"\\\"\$(sysconfdir)\\\"\" -DLOCALSTATEDIR=\"\\\"\$(localstatedir)\\\"\"  "
# -Wno-unused

AC_MSG_CHECKING(if --enable-kernel-support option is specified)
AC_ARG_ENABLE(kernel-support,
	[  --enable-kernel-support      enable Linux kernel support],
	[enable_kernel=$enable_kernel_support
	 AC_MSG_RESULT(yes)],
	[enable_kernel=default
	 AC_MSG_RESULT(no)])

AC_MSG_CHECKING(if --enable-sma-crawler option is specified)
AC_ARG_ENABLE(sma-crawler,
	[  --enable-sma-crawler      enable SMA crawler extensions],
	[enable_sma_crawler=$enable_sma_crawler
	 AC_MSG_RESULT(yes)],
	[enable_sma_crawler=default
	 AC_MSG_RESULT(no)])

AC_MSG_CHECKING(if --enable-mobile-router option is specified)
AC_ARG_ENABLE(mobile-router,
	[  --enable-mobile-router    enable mobile router server extensions],
	[enable_mobile_router=$enable_mobile_router
	 AC_MSG_RESULT(yes)],
	[enable_mobile_router=default
	 AC_MSG_RESULT(no)])

#
# Kernel detection - determines whether to build hipd for use with a HIP kernel
################################################################################

# check for pfkeyv2.h
# set kernel_support = yes/no
kernel_support=no
AC_CANONICAL_HOST
case "$host_os" in
 *linux*)
    AC_MSG_CHECKING(for Linux kernel headers)
    AC_ARG_WITH(kernel-headers,
        AC_HELP_STRING([--with-kernel-headers=/lib/modules/<uname>build/include],
			[where your Linux kernel headers are installed]),
	    [ KERNEL_INCLUDE="$with_kernel_headers"
	      CONFIGURE_AMFLAGS="--with-kernel-headers=$with_kernel_headers"
	      AC_SUBST(CONFIGURE_AMGFLAGS) ],
	    [ KERNEL_INCLUDE="/lib/modules/`uname -r`/build/include" ])
    AC_CHECK_HEADER([$KERNEL_INCLUDE/linux/pfkeyv2.h], 
    	[kernel_support=yes],
	[ AC_CHECK_HEADER(/usr/src/linux/include/linux/pfkeyv2.h,
		  [ KERNEL_INCLUDE=/usr/src/linux/include
		    kernel_support=yes],
		  [kernel_support=no])
	])
    if test "$kernel_support" = "yes"; then
    	#AC_TRY_RUN
#	AC_DEFINE(CONFIG_HIP)
	AC_COMPILE_IFELSE(
		[AC_LANG_PROGRAM([[#include "$KERNEL_INCLUDE/linux/pfkeyv2.h"]],
				 [[if (SADB_HIP_ACQUIRE < SADB_HIP_ADD) return 0; else return 99; ]])],
		 [AC_MSG_RESULT([HIP patch was found in pfkeyv2.h.])
		  kernel_support=yes],
		 [AC_MSG_RESULT([HIP patch missing from pfkeyv2.h.])
		  kernel_support=no] )
#	AC_UNDEFINE(CONFIG_HIP)
	AC_MSG_CHECKING(if kernel has HIP patch)
	AC_MSG_RESULT($kernel_support)
    fi
    #AC_SUBST(KERNEL_INCLUDE)
    ;;
  *)
    kernel_support=no
    ;;
esac

# check configure option vs detected kernel
if test "$enable_kernel_support" = "yes"; then
	if test "$kernel_support" = "no" ; then
		AC_MSG_ERROR([Kernel support requested, but HIP kernel not found! Aborting.])
	else
		want_kernel=true
	fi
else
	if test "$enable_kernel_support" = "no"; then
		want_kernel=false
	else
		# unspecified
		if test "$kernel_support" = "yes"; then
			want_kernel=true
		else
			want_kernel=false
		fi
	fi
fi

if test $want_kernel = true; then
	CFLAGS="-I$KERNEL_INCLUDE $CFLAGS"
fi

# extra check to make sure libipsec is patched if 
# we are going to compile kernel support
if test "$kernel_support" = "yes"; then
	if test "x$ac_cv_lib_ipsec_pfkey_send_hip_x1" = "xyes"; then
		AC_MSG_RESULT([Kernel and libipsec HIP patches OK!]);
	else
		AC_MSG_ERROR([Kernel has HIP patch but libipsec has not been patched with HIP patch.])
	fi
fi

#
# configure option to define SMA_CRAWLER
################################################################################
if test "$enable_sma_crawler" = "yes"; then
	want_sma_crawler=true
	CFLAGS=" -DSMA_CRAWLER $CFLAGS"

	AC_CHECK_LIB([ssl], [SSL_library_init])
	if test "x$ac_cv_lib_ssl_SSL_library_init" = "xyes"; then
		AC_MSG_RESULT([OpenSSL library found for SMA crawler extensions])
	else
		AC_MSG_ERROR([SMA crawler extensions requested but OpenSSL library not found.])
	fi

	AC_CHECK_LIB([opensc], [sc_connect_card])
	if test "x$ac_cv_lib_opensc_sc_connect_card" = "xyes"; then
		AC_MSG_RESULT([OpenSC smart card library found for SMA crawler extensions])
	else
		AC_MSG_ERROR([SMA crawler extensions requested but OpenSC smart card library not found.])
	fi
else
       want_sma_crawler=false
fi

#
# HIP mobile router server extensions 
################################################################################
# check to make sure libipq is present if we are going to compile
# mobile router support
want_mobile_router=false
if test "x$enable_mobile_router" = "xyes"; then
	AC_CHECK_LIB([ipq], [ipq_create_handle])
	if test "x$ac_cv_lib_ipq_ipq_create_handle" = "xyes"; then
		CFLAGS="$CFLAGS -DMOBILE_ROUTER"
		want_mobile_router=true
	else
		AC_MSG_ERROR([mobile router server extensions requested but libipq not found! Aborting.])
	fi
fi

#
# Mac OS X detection 
################################################################################
want_osx=false
case "${host}" in
  *-apple-darwin*)
    want_osx=true
    ;;
esac

# Automake conditionals
################################################################################
AM_CONDITIONAL(WANT_KERNEL, test x$want_kernel = xtrue)
AM_CONDITIONAL(WANT_SMA_CRAWLER, test x$want_sma_crawler = xtrue)
AM_CONDITIONAL(WANT_MOBILE_ROUTER, test x$want_mobile_router = xtrue)
AM_CONDITIONAL(WANT_OSX, test x$want_osx = xtrue)

#AC_SUBST(WANT_KERNEL, $want_kernel)

PKG_CHECK_MODULES(HIP, libxml-2.0)

AC_CONFIG_FILES([src/include/hip/hip_version.h
		 src/linux/openhip.spec
		 src/linux/deb/control
		 src/win32/install-win32/openhip.nsi
		 docs/Doxyfile
		 Makefile
                 src/Makefile
		 docs/Makefile])
AC_OUTPUT

#
# Summary message at the end of configure output. Keep this pretty.
################################################################################
AC_MSG_NOTICE([ ])
AC_MSG_NOTICE([=== Finished configuring $PACKAGE_STRING: ===])
AC_MSG_NOTICE([ ])
AC_MSG_NOTICE([    - building HIP user mode binary 'hip'])
AC_MSG_NOTICE([ ])
AC_MSG_NOTICE([    - building 'hitgen' utility])
AC_MSG_NOTICE([ ])

if test $want_kernel = true; then
	AC_MSG_NOTICE([    - building HIP daemon 'hipd' for kernel mode])
else
	AC_MSG_NOTICE([    - NOT building HIP daemon 'hipd' for kernel mode])
fi
AC_MSG_NOTICE([ ])

if test $want_sma_crawler = true; then
	AC_MSG_NOTICE([    - building HIP SMA crawler extensions])
else
	AC_MSG_NOTICE([    - NOT building HIP SMA crawler extensions])
fi
AC_MSG_NOTICE([ ])

if test $want_mobile_router = true; then
	AC_MSG_NOTICE([    - building mobile router server extensions])
else
	AC_MSG_NOTICE([    - NOT building mobile router server extensions])
fi
AC_MSG_NOTICE([ ])
