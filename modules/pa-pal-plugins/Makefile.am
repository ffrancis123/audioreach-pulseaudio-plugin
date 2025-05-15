AM_CFLAGS = -std=gnu11 @LIBPULSE_CFLAGS@
AM_CFLAGS += -I $(top_srcdir)/module-pal-card/inc/
AM_CFLAGS += -I $(top_srcdir)/module-pal-voiceui-card/
MODULE_LDFLAGS = -module -disable-static -avoid-version -shared
MODULE_LIBADD = -lpthread @LIBPULSE_LIBS@

palconfsetsdir=/etc
AM_CPPFLAGS = \
	-DPAL_CARD_DEFAULT_CONF_PATH=\"$(palconfsetsdir)\"

modlibexec_LTLIBRARIES = module-pal-card.la

module_pal_card_la_SOURCES = \
        ${top_srcdir}/module-pal-card/src/module-pal-card.c \
        ${top_srcdir}/module-pal-card/src/pal-sink.c \
        ${top_srcdir}/module-pal-card/src/pal-source.c \
        ${top_srcdir}/module-pal-card/src/pal-utils.c \
        ${top_srcdir}/module-pal-card/src/pal-config-parser.c \
        ${top_srcdir}/module-pal-card/src/module-pal-card-extn.c \
        ${top_srcdir}/module-pal-card/src/pal-jack-hdmi-out.c \
        ${top_srcdir}/module-pal-card/src/pal-jack.c \
        ${top_srcdir}/module-pal-card/src/pal-format-detection.c \
        ${top_srcdir}/module-pal-card/src/bt-a2dp-split.c \
        ${top_srcdir}/module-pal-card/src/hfp.c \
        ${top_srcdir}/module-pal-card/src/pal-loopback.c \
        ${top_srcdir}/module-pal-card/src/pal-jack-external.c

module_pal_card_la_CFLAGS = $(PAL_CFLAGS) $(AGM_CFLAGS) $(AM_CFLAGS) @DBUS_CFLAGS@ -DPA_PACKAGE_VERSION=\""$(PKG_VER)"\"
if COMPRESS_AUDIO_NOT_SUPPORTED
module_pal_card_la_CFLAGS += -DPAL_DISABLE_COMPRESS_AUDIO_SUPPORT
endif
module_pal_card_la_LDFLAGS = $(MODULE_LDFLAGS)
module_pal_card_la_LIBADD = $(MODULE_LIBADD) @DBUS_LIBS@ -lpal -lagm

if CUTILS_SUPPORTED
module_pal_card_la_CFLAGS += -DPAL_USES_CUTILS
module_pal_card_la_LIBADD += -lcutils -llog
endif

module_pal_card_la_list = \
                          ${top_srcdir}/module-pal-card/configs/default.conf \
                          ${top_srcdir}/module-pal-card/configs/qcm6490-idp-snd-card.conf \
                          ${top_srcdir}/module-pal-card/configs/qcm6490-rb3-snd-card.conf \
                          ${top_srcdir}/module-pal-card/configs/qcm6490-rb3-vc-snd-card.conf \
                          ${top_srcdir}/module-pal-card/configs/qcm6490-rb3-vision-snd-card.conf \
                          ${top_srcdir}/module-pal-card/configs/qcs8300-ridesx-snd-card.conf \
                          ${top_srcdir}/module-pal-card/configs/qcs9100-ridesx-snd-card.conf

if VUI_ENABLED
modlibexec_LTLIBRARIES += module-pal-voiceui-card.la
module_pal_voiceui_card_la_SOURCES = ${top_srcdir}/module-pal-voiceui-card/module-pal-voiceui-card.c
module_pal_voiceui_card_la_CFLAGS = $(AM_CFLAGS) $(DBUS_CFLAGS) $(PAL_CFLAGS) @VUI_INTF_HEADERS_CFLAGS@ -DPA_PACKAGE_VERSION=\""${PKG_VER}"\"
module_pal_voiceui_card_la_LDFLAGS = $(MODULE_LDFLAGS)
module_pal_voiceui_card_la_LIBADD = $(MODULE_LIBADD) $(DBUS_LIBS) -lpal
endif

# install essential xml files under /etc
root_etcdir      = "/etc"
root_etc_SCRIPTS = $(module_pal_card_la_list)
install-data-hook:
	chmod  go+r $(DESTDIR)$(root_etcdir)/qcm6490-idp-snd-card.conf
	chmod  go+r $(DESTDIR)$(root_etcdir)/qcm6490-rb3-snd-card.conf
	chmod  go+r $(DESTDIR)$(root_etcdir)/qcm6490-rb3-vc-snd-card.conf
	chmod  go+r $(DESTDIR)$(root_etcdir)/qcm6490-rb3-vision-snd-card.conf
	chmod  go+r $(DESTDIR)$(root_etcdir)/qcs8300-ridesx-snd-card.conf
	chmod  go+r $(DESTDIR)$(root_etcdir)/qcs9100-ridesx-snd-card.conf
