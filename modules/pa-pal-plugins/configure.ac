#                                               -*- Autoconf -*-
# configure.ac -- Autoconf script for pulseaudio pal plugin
#

# Process this file with autoconf to produce a configure script.

# Requires autoconf tool later than 2.61
AC_PREREQ([2.69])
AC_INIT([pa-pal-plugins],1.0.0)
# Does not strictly follow GNU Coding standards
AM_INIT_AUTOMAKE([foreign])
# Disables auto rebuilding of configure, Makefile.ins
#AM_MAINTAINER_MODE
# defines some macros variable to be included by source
AC_CONFIG_HEADERS([config.h])
# defines some macros variable to be included by source
AC_CONFIG_MACRO_DIR([m4])

# Checks for programs.
AM_PROG_AS
AC_PROG_CC
AM_PROG_CC_C_O
AC_PROG_CXX
AC_PROG_LIBTOOL
AC_PROG_AWK
AC_PROG_CPP
AC_PROG_INSTALL
AC_PROG_LN_S
AC_PROG_MAKE_SET
PKG_PROG_PKG_CONFIG

PKG_CHECK_MODULES([GIO], [gio-2.0])
AC_SUBST([GIO_CFLAGS])
AC_SUBST([GIO_LIBS])

PKG_CHECK_MODULES([AGM], [agm])
AC_SUBST([AGM_CFLAGS])
AC_SUBST([AGM_LIBS])

PKG_CHECK_MODULES([PAL], [pal])
AC_SUBST([PAL_CFLAGS])
AC_SUBST([PAL_LIBS])

PKG_CHECK_MODULES([LIBPULSE], [libpulse])
AC_SUBST([LIBPULSE_CFLAGS])
AC_SUBST([LIBPULSE_LIBS])

PKG_CHECK_MODULES([DBUS], [dbus-1])
AC_SUBST([DBUS_CFLAGS])
AC_SUBST([DBUS_LIBS])

AC_ARG_WITH([pa_version],
                AS_HELP_STRING([--without-pa-version],[Substituting pulseaudio version as default]),[],[with_pa_version=2.0])
AS_IF([test "x$with_pa_version" != "xno"], [PKG_VER="$with_pa_version"])
AC_SUBST(PKG_VER)

AC_ARG_WITH([cutils],
    AS_HELP_STRING([Use cutils (default is no)]),
     [with_cutils=$withval],
     [with_cutils=no])
AM_CONDITIONAL([CUTILS_SUPPORTED], [test "x${with_cutils}" = "xyes"])

AC_ARG_WITH([pa-support-card-status], AC_HELP_STRING([--with-pa-support-card-status], [enable sound-card status support]))
AM_CONDITIONAL(PAL_CARD_STATUS_SUPPORTED, test "x${with_pa_support_card_status}" = "xyes")
AC_SUBST(PAL_CARD_STATUS_SUPPORTED)

AC_ARG_WITH([pa-compress-audio-support], AC_HELP_STRING([--without-pa-compress-audio-support], [disables compress offload support]))
AM_CONDITIONAL(COMPRESS_AUDIO_NOT_SUPPORTED, test "x${with_pa_compress_audio_support}" = "xno")
AC_SUBST(COMPRESS_AUDIO_NOT_SUPPORTED)

AC_ARG_WITH([vui],
    AS_HELP_STRING([compile vui (default is no)]),
    [with_vui=$withval],
    [with_vui=no])

if (test "x${with_vui}" = "xyes"); then
    PKG_CHECK_MODULES([VUI_INTF_HEADERS],[vui-intf-header])
    AC_SUBST([VUI_INTF_HEADERS_CFLAGS])
fi

AM_CONDITIONAL([VUI_ENABLED], [test "x${with_VUI}" = "xyes"])

AC_ARG_WITH(
        [module-dir],
        AS_HELP_STRING([--with-module-dir],[Directory where to install the modules to (defaults to ${libdir}/pulseaudio/modules]),
        [modlibexecdir=$withval], [modlibexecdir="${libdir}/pulseaudio/modules"])
AC_SUBST(modlibexecdir)

AC_CONFIG_FILES([ \
                         Makefile \
                ])

AC_OUTPUT
