#!/bin/sh

# Test for prerequisites aclocal, automake, autoconf
command -v aclocal >/dev/null 2>&1 || { echo >&2 "Command 'aclocal' not found, aborting."; exit 1;}
command -v automake >/dev/null 2>&1 || { echo >&2 "Command 'automake' not found, aborting."; exit 1;}
command -v autoconf >/dev/null 2>&1 || { echo >&2 "Command 'autoconf' not found, aborting."; exit 1;}

# libtool is only used to build the configuration libraries associated
#  with the --enable-vpls configure option
if [ "$1a" = "--enable-vplsa" ]; then
	LIBTOOLIZE_MSG="echo '(1.5/3) Running libtoolize...'"
	LIBTOOLIZE="libtoolize --force --copy --automake"
	CONFOPTS=" --enable-vpls"
	if [ -e src/util/Makefile.am.disabled ]; then
		mv src/util/Makefile.am.disabled src/util/Makefile.am
	fi
	mv configure.ac configure.ac.orig
	sed -e "s,#AC_PROG_LIBTOOL,AC_PROG_LIBTOOL," configure.ac.orig > configure.ac
elif [ "$1a" = "a" ]; then
	LIBTOOLIZE_MSG=""
	LIBTOOLIZE=""
	CONFOPTS=""
	if [ ! -e src/util/Makefile.am.disabled ]; then
		mv src/util/Makefile.am src/util/Makefile.am.disabled
		touch src/util/Makefile.am
	fi
	if [ -e configure.ac.orig ]; then
		mv configure.ac.orig configure.ac
	fi
else
	echo "usage: ./bootstrap.sh [--enable-vpls]"
	exit 1;
fi

if [ -d /usr/local/share/aclocal ]; then
	EXTRA_INC="-I /usr/local/share/aclocal"
else
	EXTRA_INC=""
fi;

echo "(1/3) Running aclocal..." && aclocal $EXTRA_INC \
    && $LIBTOOLIZE_MSG && $LIBTOOLIZE \
    && echo "(2/3) Running automake..." \
    && automake --add-missing --copy --foreign \
    && echo "(3/3) Running autoconf..." && autoconf \
    && echo "" \
    && echo "You are now ready to run \"./configure\"$CONFOPTS."

