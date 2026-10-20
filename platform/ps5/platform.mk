# The PS5: a native ShadowMountPlus title, built with the toolchain of
# ps5-native-app-boilerplate, which is vendored in ps5/.
#
#   brew install llvm@18 coreutils gnu-sed bash cmake sdl2 libmicrohttpd
#   cd ps5 && make          # once, to build the boilerplate's own tooling
#
# tools/build-ps5.sh does the real work: it drives the boilerplate's
# compiler wrapper, converts and signs eboot.bin, and lays out the title
# folder. This fragment exists to generate what it needs and hand over.
.DEFAULT_GOAL := all

TARGET    := chiral-ftp
TITLE_ID  := PPSA96024

PLATFORM_CPP := platform/ps5/backend.cpp platform/ps5/elevation/elevation.cpp
PLATFORM_C   := platform/ps5/system.c platform/ps5/runtime.c platform/ps5/net.c
GENERATED    := $(FONTS) $(WEB_GEN) $(GEN_I18N) gen/icon0.png.c

# The dashboard icon is served by the web app as its favicon too.
gen/icon0.png.c: platform/ps5/sce_sys/icon0.png tools/gen-asset-module.py | gen
	$(PYTHON) tools/gen-asset-module.py --path icon0.png $< > $@

# Everything the build script needs to know, so it needn't duplicate any
# of the source lists above.
export CHIRAL_VERSION   := $(VERSION)
export CHIRAL_TITLE_ID  := $(TITLE_ID)
export CHIRAL_ENGINE    := $(ENGINE)
export CHIRAL_WEB       := $(WEB_SRCS)
export CHIRAL_APP       := $(APP)
export CHIRAL_IMGUI     := $(IMGUI)
export CHIRAL_PLATFORM_C   := $(PLATFORM_C)
export CHIRAL_PLATFORM_CPP := $(PLATFORM_CPP)
export CHIRAL_GENERATED := $(GENERATED)
export CHIRAL_INCLUDES  := app engine web vendor/imgui gen platform/ps5

.PHONY: all dist release deploy

all: dist

dist: $(GENERATED)
	VERSION=$(VERSION) ./tools/build-ps5.sh

release: dist
	VERSION=$(VERSION) PYTHON=$(PYTHON) ./tools/make-release.sh ps5

PS5_PORT ?= 2121
deploy: dist
	@[ -n "$(PS5_HOST)" ] || { echo "usage: make PLATFORM=ps5 deploy PS5_HOST=<ip> [PS5_PORT=2121]"; exit 2; }
	$(PYTHON) tools/deploy-ftp.py dist/$(TITLE_ID) --host $(PS5_HOST) --port $(PS5_PORT)
