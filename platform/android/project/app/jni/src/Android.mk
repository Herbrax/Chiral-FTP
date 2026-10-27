# chiral-ftp's native library, libmain.so: the shared engine, UI and web
# app straight from the tree (CHIRAL_ROOT, passed in by
# tools/build-android.sh), Android's platform layer, and the libraries
# tools/build-android-deps.sh, tools/build-android-libssh.sh and
# tools/build-android-mhd.sh built for each ABI.
LOCAL_PATH := $(call my-dir)

DEPS := $(CHIRAL_ROOT)/.deps/android/$(TARGET_ARCH_ABI)

include $(CLEAR_VARS)
LOCAL_MODULE := ssh2
LOCAL_SRC_FILES := $(DEPS)/lib/libssh2.a
include $(PREBUILT_STATIC_LIBRARY)
include $(CLEAR_VARS)
LOCAL_MODULE := ssh
LOCAL_SRC_FILES := $(DEPS)/lib/libssh.a
include $(PREBUILT_STATIC_LIBRARY)
include $(CLEAR_VARS)
LOCAL_MODULE := mbedtls
LOCAL_SRC_FILES := $(DEPS)/lib/libmbedtls.a
include $(PREBUILT_STATIC_LIBRARY)
include $(CLEAR_VARS)
LOCAL_MODULE := mbedx509
LOCAL_SRC_FILES := $(DEPS)/lib/libmbedx509.a
include $(PREBUILT_STATIC_LIBRARY)
include $(CLEAR_VARS)
LOCAL_MODULE := mbedcrypto
LOCAL_SRC_FILES := $(DEPS)/lib/libmbedcrypto.a
include $(PREBUILT_STATIC_LIBRARY)
include $(CLEAR_VARS)
LOCAL_MODULE := everest
LOCAL_SRC_FILES := $(DEPS)/lib/libeverest.a
include $(PREBUILT_STATIC_LIBRARY)
include $(CLEAR_VARS)
LOCAL_MODULE := p256m
LOCAL_SRC_FILES := $(DEPS)/lib/libp256m.a
include $(PREBUILT_STATIC_LIBRARY)

include $(CLEAR_VARS)
LOCAL_MODULE := microhttpd
LOCAL_SRC_FILES := $(DEPS)/lib/libmicrohttpd.a
include $(PREBUILT_STATIC_LIBRARY)

include $(CLEAR_VARS)
LOCAL_MODULE := main

WEB_PAGES := index.html app.css app.js files.html files.css files.js mark.svg mark-light.svg plexmono.ttf

ENGINE := ftp listparse session queue localfs settings log strbuf form notify sftp remote_ops ftpd sftpd fsjobs discovery
LOCAL_SRC_FILES := \
  $(foreach f,$(ENGINE),$(CHIRAL_ROOT)/engine/$(f).c) \
  $(CHIRAL_ROOT)/web/websrv.c $(CHIRAL_ROOT)/web/api.c $(CHIRAL_ROOT)/web/asset.c \
  $(foreach f,$(WEB_PAGES),$(CHIRAL_ROOT)/gen/$(f).c) \
  $(CHIRAL_ROOT)/app/main.cpp $(CHIRAL_ROOT)/app/ui.cpp $(CHIRAL_ROOT)/app/json.cpp \
  $(CHIRAL_ROOT)/app/platform_common.cpp $(CHIRAL_ROOT)/app/raster.cpp $(CHIRAL_ROOT)/app/i18n.cpp \
  $(CHIRAL_ROOT)/vendor/imgui/imgui.cpp $(CHIRAL_ROOT)/vendor/imgui/imgui_draw.cpp \
  $(CHIRAL_ROOT)/vendor/imgui/imgui_tables.cpp $(CHIRAL_ROOT)/vendor/imgui/imgui_widgets.cpp \
  $(CHIRAL_ROOT)/gen/font_regular.c $(CHIRAL_ROOT)/gen/font_bold.c $(CHIRAL_ROOT)/gen/font_mono.c \
  $(CHIRAL_ROOT)/gen/i18n_table.c \
  $(CHIRAL_ROOT)/platform/android/backend.cpp $(CHIRAL_ROOT)/platform/android/system.c

LOCAL_C_INCLUDES := $(LOCAL_PATH)/../SDL/include $(CHIRAL_ROOT)/platform/android $(CHIRAL_ROOT)/engine \
  $(CHIRAL_ROOT)/app $(CHIRAL_ROOT)/web $(CHIRAL_ROOT)/vendor/imgui $(CHIRAL_ROOT)/gen $(DEPS)/include
LOCAL_CFLAGS := -O2 -Wall -D_GNU_SOURCE -DVERSION_TAG=\"$(CHIRAL_VERSION)\" -DIMGUI_USER_CONFIG=\"imgui_config.h\"
LOCAL_CPPFLAGS := -std=gnu++17 -fno-rtti
LOCAL_SHARED_LIBRARIES := SDL2
LOCAL_STATIC_LIBRARIES := microhttpd ssh ssh2 mbedtls mbedx509 mbedcrypto everest p256m
LOCAL_LDLIBS := -lGLESv1_CM -lGLESv2 -llog -landroid
LOCAL_SHORT_COMMANDS := true

include $(BUILD_SHARED_LIBRARY)
