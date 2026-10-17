# chiral-ftp - FileZilla-style FTP/SFTP client for the PS5 (native ShadowMountPlus title).
#
#   make ps5     dist/PPSA96024/ and dist/chiral-ftp-<version>.zip
#   make deploy PS5_HOST=192.168.18.18   send the build to the PS5 (ftpsrv, port 2121)
#   make release  release/: the zip, SHA256SUMS and notes for a GitHub release
#   make host    ./chiral-ftp-host, the same app in a window (needs brew sdl2)
#   make test    unit tests on the computer

VERSION := 1.0
PYTHON ?= python3
HOST_CC ?= cc

ENGINE := engine/ftp.c engine/listparse.c engine/session.c engine/queue.c engine/localfs.c \
          engine/settings.c engine/log.c engine/strbuf.c engine/form.c engine/notify.c \
          engine/sftp.c engine/remote_ops.c engine/ps5_net.c engine/ftpd.c engine/fsjobs.c
WEB := web/websrv.c web/api.c web/asset.c
WEB_ASSETS := web/assets/index.html web/assets/app.css web/assets/app.js web/assets/files.html \
              web/assets/files.css web/assets/files.js
WEB_GEN := $(patsubst web/assets/%,gen/%.c,$(WEB_ASSETS)) gen/icon0.png.c gen/mark.svg.c gen/mark-light.svg.c \
           gen/plexmono.ttf.c
APP := app/main.cpp app/ui.cpp app/json.cpp app/platform_common.cpp app/raster.cpp app/i18n.cpp
IMGUI := vendor/imgui/imgui.cpp vendor/imgui/imgui_draw.cpp vendor/imgui/imgui_tables.cpp \
         vendor/imgui/imgui_widgets.cpp
FONTS := gen/font_regular.c gen/font_bold.c gen/font_mono.c
# Translations: i18n/translations.json, checked against every tr()/trf()/N_()
# string in these sources (a missing one fails the build).
I18N_SOURCES := $(wildcard app/*.cpp) engine/ftpd.c engine/fsjobs.c
HEADERS := $(wildcard app/*.h engine/*.h web/*.h)

gen:
	mkdir -p gen
gen/font_regular.c: assets/fonts/NotoSans-Regular.ttf tools/bin2c.py | gen
	$(PYTHON) tools/bin2c.py font_regular_ttf $< > $@
gen/font_bold.c: assets/fonts/NotoSans-SemiBold.ttf tools/bin2c.py | gen
	$(PYTHON) tools/bin2c.py font_bold_ttf $< > $@
gen/i18n.c: i18n/translations.json tools/gen-i18n.py $(I18N_SOURCES) | gen
	$(PYTHON) tools/gen-i18n.py i18n/translations.json $@ $(I18N_SOURCES)
gen/font_mono.c: assets/fonts/IBMPlexMono-Medium.ttf tools/bin2c.py | gen
	$(PYTHON) tools/bin2c.py font_mono_ttf $< > $@
# The web UI, gzipped into C arrays that register themselves at startup.
gen/%.c: web/assets/% tools/gen-asset-module.py | gen
	$(PYTHON) tools/gen-asset-module.py --path $* $< > $@
gen/icon0.png.c: sce_sys/icon0.png tools/gen-asset-module.py | gen
	$(PYTHON) tools/gen-asset-module.py --path icon0.png $< > $@
# The brand: the mark (dark and light backgrounds) and the wordmark's font.
gen/mark.svg.c: assets/brand/chiral-ftp-mark.svg tools/gen-asset-module.py | gen
	$(PYTHON) tools/gen-asset-module.py --path mark.svg $< > $@
gen/mark-light.svg.c: assets/brand/chiral-ftp-mark-light.svg tools/gen-asset-module.py | gen
	$(PYTHON) tools/gen-asset-module.py --path mark-light.svg $< > $@
gen/plexmono.ttf.c: assets/fonts/IBMPlexMono-Medium.ttf tools/gen-asset-module.py | gen
	$(PYTHON) tools/gen-asset-module.py --path fonts/IBMPlexMono-Medium.ttf $< > $@

# ---- computer build -------------------------------------------------------
HOST_DEPS := .deps/host
HOST_OBJ := .build/host
HOST_DEFS := -DVERSION_TAG=\"$(VERSION)-host\"
HOST_MHD := `pkg-config libmicrohttpd --cflags`
HOST_CXXFLAGS := -std=c++17 -O2 -g -Wall -DIMGUI_USER_CONFIG=\"imgui_config.h\" -Iapp -Iengine -Iweb -Ivendor/imgui `sdl2-config --cflags` $(HOST_MHD) $(HOST_DEFS)
HOST_CFLAGS := -O2 -g -Wall -Werror -Iengine -Iweb -I$(HOST_DEPS)/include $(HOST_MHD) -pthread $(HOST_DEFS)
HOST_OBJS := $(addprefix $(HOST_OBJ)/,$(APP:.cpp=.o) app/backend_sdl.o $(IMGUI:.cpp=.o) $(ENGINE:.c=.o) $(WEB:.c=.o) \
               $(FONTS:.c=.o) $(WEB_GEN:.c=.o) gen/i18n.o)

$(HOST_OBJ)/%.o: %.cpp $(HEADERS)
	@mkdir -p $(dir $@)
	$(HOST_CC) -x c++ $(HOST_CXXFLAGS) -c -o $@ $<
$(HOST_OBJ)/%.o: %.c $(HEADERS)
	@mkdir -p $(dir $@)
	$(HOST_CC) $(HOST_CFLAGS) -c -o $@ $<

host: chiral-ftp-host
chiral-ftp-host: $(HOST_OBJS)
	$(HOST_CC) -o $@ $^ $(HOST_DEPS)/lib/libssh2.a $(HOST_DEPS)/lib/libmbedcrypto.a `sdl2-config --libs` \
	  `pkg-config libmicrohttpd --libs` -lc++ -pthread

tests/test_listparse: tests/test_listparse.c engine/listparse.c engine/ftp.h
	$(HOST_CC) -O1 -g -Wall -Werror -Iengine -o $@ tests/test_listparse.c engine/listparse.c
test: tests/test_listparse
	./tests/test_listparse

# ---- PS5 build (native title) ---------------------------------------------
ps5: $(FONTS) $(WEB_GEN) gen/i18n.c
	VERSION=$(VERSION) ./tools/build-ps5.sh

release: ps5
	VERSION=$(VERSION) ./tools/make-release.sh

PS5_PORT ?= 2121
deploy: ps5
	@[ -n "$(PS5_HOST)" ] || { echo "usage: make deploy PS5_HOST=<PS5 IP> [PS5_PORT=2121]"; exit 2; }
	$(PYTHON) tools/deploy-ftp.py dist/PPSA96024 --host $(PS5_HOST) --port $(PS5_PORT)

clean:
	rm -rf .build gen dist release chiral-ftp-host tests/test_listparse data

.PHONY: host test ps5 deploy release clean
