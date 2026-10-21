# This computer: an SDL window. Linux, macOS (including Apple silicon) and
# Windows under MSYS2. Needs SDL2, libmicrohttpd and pkg-config, plus
# libssh2 and mbedTLS -- tools/build-sftp-deps.sh builds those locally --
# and libssh for the SFTP server.
.DEFAULT_GOAL := all

TARGET  := chiral-ftp
BUILD   := .build/sdl
HOST_CC ?= cc
DEPS    := .deps/host

PLATFORM_CPP := platform/sdl/backend.cpp
# The SFTP server (CHIRAL_HAS_SFTPD): this build only, on libssh.
PLATFORM_C   := platform/sdl/system.c engine/sftpd.c
PLATFORM_MM  :=
GENERATED    := $(FONTS) $(WEB_GEN) $(GEN_I18N)

# macOS: the menu bar and the folder chooser (Objective-C++), and the names
# it gives its network interfaces ("Wi-Fi") for the settings screen.
# The C++ library: libc++ on macOS, libstdc++ with Linux's GCC.
CXX_LIB      := -lstdc++
ifeq ($(shell uname -s),Darwin)
PLATFORM_MM  := platform/sdl/macos.mm
OS_LIBS      := -framework SystemConfiguration -framework CoreFoundation
CXX_LIB      := -lc++
endif

MHD_CFLAGS := $(shell pkg-config libmicrohttpd --cflags)
MHD_LIBS   := $(shell pkg-config libmicrohttpd --libs)
SDL_CFLAGS := $(shell sdl2-config --cflags)
SDL_LIBS   := $(shell sdl2-config --libs)
SSH_CFLAGS := $(shell pkg-config libssh --cflags)
SSH_LIBS   := $(shell pkg-config libssh --libs)
# Text through FreeType, hinted, instead of ImGui's own rasterizer: on a
# screen at one pixel per point, unhinted stems fall between pixels and the
# text looks soft. This build only; the consoles keep stb_truetype.
FT_CFLAGS  := $(shell pkg-config freetype2 --cflags) -DIMGUI_ENABLE_FREETYPE -Ivendor/imgui/misc/freetype
FT_LIBS    := $(shell pkg-config freetype2 --libs)

DEFINES  := -DVERSION_TAG=\"$(VERSION)-sdl\" -DIMGUI_USER_CONFIG=\"imgui_config.h\"
INCLUDE  := -Iapp -Iengine -Iweb -Ivendor/imgui -Igen -Iplatform/sdl -I$(DEPS)/include
CFLAGS   := -O2 -g -Wall -pthread $(DEFINES) $(INCLUDE) $(MHD_CFLAGS) $(SSH_CFLAGS)
CXXFLAGS := -std=c++17 -O2 -g -Wall $(DEFINES) $(INCLUDE) $(MHD_CFLAGS) $(SDL_CFLAGS) $(FT_CFLAGS)

SRC_C   := $(ENGINE) $(WEB_SRCS) $(PLATFORM_C) $(GENERATED)
SRC_CPP := $(APP) $(IMGUI) $(PLATFORM_CPP) vendor/imgui/misc/freetype/imgui_freetype.cpp
OBJS    := $(addprefix $(BUILD)/,$(SRC_CPP:.cpp=.o) $(SRC_C:.c=.o) $(PLATFORM_MM:.mm=.o))
HEADERS := $(wildcard app/*.h app/*.inc engine/*.h web/*.h platform/sdl/*.h)

$(BUILD)/%.o: %.cpp $(HEADERS) | $(GENERATED)
	@mkdir -p $(dir $@)
	$(HOST_CC) -x c++ $(CXXFLAGS) -c -o $@ $<
$(BUILD)/%.o: %.c $(HEADERS) | $(GENERATED)
	@mkdir -p $(dir $@)
	$(HOST_CC) $(CFLAGS) -c -o $@ $<
$(BUILD)/%.o: %.mm $(HEADERS) | $(GENERATED)
	@mkdir -p $(dir $@)
	$(HOST_CC) -x objective-c++ -fobjc-arc $(CXXFLAGS) -c -o $@ $<

.PHONY: all
all: $(TARGET)
$(TARGET): $(OBJS)
	$(HOST_CC) -o $@ $^ $(DEPS)/lib/libssh2.a $(DEPS)/lib/libmbedcrypto.a \
	  $(SDL_LIBS) $(MHD_LIBS) $(SSH_LIBS) $(FT_LIBS) $(OS_LIBS) $(CXX_LIB) -pthread
	@echo "built ./$(TARGET)"

# release/: the Mac app, zipped, next to the consoles' files
# (tools/make-release.sh). On Linux the AppImage comes from tools/make-appimage.sh.
.PHONY: release
release: app
	VERSION=$(VERSION) PYTHON=$(PYTHON) ./tools/make-release.sh mac

# macOS only: dist/chiral-ftp.app, with its Homebrew libraries inside.
.PHONY: app
app: $(TARGET)
	$(PYTHON) tools/make-macos-app.py $(TARGET) $(VERSION)
