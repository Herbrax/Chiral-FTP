# chiral-ftp -- one tree, three platforms.
#
#   make PLATFORM=switch     dist/chiral-ftp/chiral-ftp.nro
#   make PLATFORM=ps5        dist/PPSA96024/ and the release zip
#   make PLATFORM=sdl        ./chiral-ftp, the same app in a window
#   make test                the unit tests (on this computer)
#
# PLATFORM defaults to sdl, because that is the one every machine can build
# and it is where most of the UI is developed. The consoles need their
# toolchains: see README.md.
#
# Everything shared lives in engine/, web/, app/ and vendor/; everything
# that differs lives in platform/<name>/ behind two contracts --
# engine/system.h for storage, network and locale, app/backend.h for the
# screen, the pad and the keyboard. No file outside platform/ asks which
# platform it is on.

VERSION := 1.1
PLATFORM ?= sdl

# The project root, kept across the recursive second pass some platforms
# use (where CURDIR becomes the build directory). Exported, so ?= below
# picks up the inherited value rather than re-deriving it wrongly.
CHIRAL_ROOT ?= $(CURDIR)
export CHIRAL_ROOT
PYTHON ?= python3

VALID_PLATFORMS := ps5 switch sdl
ifeq ($(filter $(PLATFORM),$(VALID_PLATFORMS)),)
$(error PLATFORM must be one of: $(VALID_PLATFORMS) (got "$(PLATFORM)"))
endif

#---------------------------------------------------------------------------
# shared sources
#---------------------------------------------------------------------------
ENGINE := engine/ftp.c engine/listparse.c engine/session.c engine/queue.c \
          engine/localfs.c engine/settings.c engine/log.c engine/strbuf.c \
          engine/form.c engine/notify.c engine/sftp.c engine/remote_ops.c \
          engine/ftpd.c engine/fsjobs.c engine/discovery.c
# WEB_SRCS, not WEB: WEB is the on/off flag a build passes in
# (make WEB=0), and one name for both silently selected the stub.
WEB_SRCS := web/websrv.c web/api.c web/asset.c
WEB_STUB := web/stub/websrv_stub.c
APP     := app/main.cpp app/ui.cpp app/json.cpp app/platform_common.cpp \
           app/raster.cpp app/i18n.cpp
IMGUI   := vendor/imgui/imgui.cpp vendor/imgui/imgui_draw.cpp \
           vendor/imgui/imgui_tables.cpp vendor/imgui/imgui_widgets.cpp

# Generated: the fonts, the web UI and the translation table, as C arrays.
WEB_ASSETS := web/assets/index.html web/assets/app.css web/assets/app.js \
              web/assets/files.html web/assets/files.css web/assets/files.js
WEB_GEN    := $(patsubst web/assets/%,gen/%.c,$(WEB_ASSETS)) \
              gen/mark.svg.c gen/mark-light.svg.c gen/plexmono.ttf.c
FONTS      := gen/font_regular.c gen/font_bold.c gen/font_mono.c
# Every tr()/trf()/N_() string in these must have an entry in
# i18n/translations.json, in every language, or the build stops.
I18N_SOURCES := $(APP) $(wildcard platform/*/*.cpp) $(wildcard platform/*/*.mm) \
                engine/ftpd.c engine/sftpd.c engine/fsjobs.c app/ui_desktop.inc
GEN_I18N   := gen/i18n_table.c

include $(CHIRAL_ROOT)/platform/$(PLATFORM)/platform.mk

#---------------------------------------------------------------------------
# generated sources
#---------------------------------------------------------------------------
gen:
	@mkdir -p gen
gen/font_regular.c: assets/fonts/NotoSans-Regular.ttf tools/bin2c.py | gen
	$(PYTHON) tools/bin2c.py font_regular_ttf $< > $@
gen/font_bold.c: assets/fonts/NotoSans-SemiBold.ttf tools/bin2c.py | gen
	$(PYTHON) tools/bin2c.py font_bold_ttf $< > $@
gen/font_mono.c: assets/fonts/IBMPlexMono-Medium.ttf tools/bin2c.py | gen
	$(PYTHON) tools/bin2c.py font_mono_ttf $< > $@
gen/i18n_table.c: i18n/translations.json tools/gen-i18n.py $(I18N_SOURCES) | gen
	$(PYTHON) tools/gen-i18n.py i18n/translations.json $@ $(I18N_SOURCES)
gen/%.c: web/assets/% tools/gen-asset-module.py | gen
	$(PYTHON) tools/gen-asset-module.py --path $* $< > $@
gen/mark.svg.c: assets/brand/chiral-ftp-mark.svg tools/gen-asset-module.py | gen
	$(PYTHON) tools/gen-asset-module.py --path mark.svg $< > $@
gen/mark-light.svg.c: assets/brand/chiral-ftp-mark-light.svg tools/gen-asset-module.py | gen
	$(PYTHON) tools/gen-asset-module.py --path mark-light.svg $< > $@
gen/plexmono.ttf.c: assets/fonts/IBMPlexMono-Medium.ttf tools/gen-asset-module.py | gen
	$(PYTHON) tools/gen-asset-module.py --path fonts/IBMPlexMono-Medium.ttf $< > $@

# The icon hbmenu shows, committed because a .nro whose icon is not exactly
# a 256x256 baseline JPEG falls back to libnx's default without saying so,
# and because building should not need a Python imaging library.
icon:
	$(PYTHON) tools/render-icon.py assets/brand/icon.jpg

#---------------------------------------------------------------------------
# the unit tests build for this computer whatever PLATFORM says
#---------------------------------------------------------------------------
tests/test_listparse: tests/test_listparse.c engine/listparse.c engine/ftp.h
	$(or $(HOST_CC),cc) -O1 -g -Wall -Werror -Iengine -Iplatform/sdl -o $@ \
	  tests/test_listparse.c engine/listparse.c
test: tests/test_listparse check
	./tests/test_listparse

# Every platform implements both contracts, and no shared file names a
# platform. Needs no console toolchain, so it runs anywhere -- which is the
# point: three functions once existed for two platforms out of three and
# nothing noticed until a link failed.
check:
	$(PYTHON) tools/check-platforms.py
	$(PYTHON) tools/content-version.py --check $(VERSION) platform/ps5/sce_sys/param.json

clean:
	rm -rf .build gen dist release chiral-ftp chiral-ftp.exe tests/test_listparse data \
	       chiral-ftp.nro chiral-ftp.nacp chiral-ftp.elf chiral-ftp.map

.PHONY: all test check clean icon
