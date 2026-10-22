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
# The C++ library: libc++ on macOS, libstdc++ with Linux's GCC -- and libm,
# which Linux keeps apart (macOS has it in libSystem).
CXX_LIB      := -lstdc++ -lm
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

DEFINES  := -DVERSION_TAG=\"$(VERSION)\" -DIMGUI_USER_CONFIG=\"imgui_config.h\"
INCLUDE  := -Iapp -Iengine -Iweb -Ivendor/imgui -Igen -Iplatform/sdl -I$(DEPS)/include
CFLAGS   := -O2 -g -Wall -pthread $(DEFINES) $(INCLUDE) $(MHD_CFLAGS) $(SSH_CFLAGS)
CXXFLAGS := -std=c++17 -O2 -g -Wall $(DEFINES) $(INCLUDE) $(MHD_CFLAGS) $(SDL_CFLAGS) $(FT_CFLAGS)

# Windows, built in MSYS2's UCRT64 shell: one chiral-ftp.exe that runs
# with nothing installed beside it -- every library linked in statically.
# The POSIX the shared code is written against (sockets, paths, folders)
# comes from win32/posix.h, which the compiler puts at the top of every
# file, and win32/include, which stands in for the headers MinGW lacks.
ifneq ($(findstring _NT-,$(shell uname -s)),)
WINDOWS      := 1
TARGET       := chiral-ftp.exe
PLATFORM_C   += platform/sdl/win32/posix.c
PLATFORM_CPP += platform/sdl/win32/windows.cpp
# IMGUI_DISABLE_TIME_FUNCTIONS: imgui.cpp brings its own localtime_r() on
# Windows, which would collide with MinGW's; all it is for is a session
# date this program doesn't use. SDL_MAIN_HANDLED, and no -Dmain=SDL_main
# from sdl2-config: app/main.cpp's main() stays main(), which MinGW starts
# in a windowed program too, rather than becoming SDL_main for SDL2main's
# WinMain (backend.cpp tells SDL).
WIN_FLAGS    := -include platform/sdl/win32/posix.h -Iplatform/sdl/win32/include \
                -D_FILE_OFFSET_BITS=64 -DLIBSSH_STATIC -DIMGUI_DISABLE_TIME_FUNCTIONS \
                -DSDL_MAIN_HANDLED
CFLAGS       += $(WIN_FLAGS)
CXXFLAGS     := $(filter-out -Dmain=SDL_main,$(CXXFLAGS)) $(WIN_FLAGS)
SDL_LIBS     := $(filter-out -lSDL2main,$(shell pkg-config sdl2 --static --libs))
MHD_LIBS     := $(shell pkg-config libmicrohttpd --static --libs)
SSH_LIBS     := $(shell pkg-config libssh --static --libs) -lcrypto -lz
FT_LIBS      := $(shell pkg-config freetype2 --static --libs)
OS_LIBS      := -lws2_32 -liphlpapi -lole32 -luuid -lshell32 -lcrypt32 -lbcrypt
CXX_LIB      := -static -lstdc++ -lwinpthread
WIN_RES      := $(BUILD)/chiral-ftp.res.o
# 1.1 -> 1,1,0,0 for the version resource.
WIN_VER      := $(subst ., ,$(VERSION)) 0 0 0
WIN_VERNUM   := $(word 1,$(WIN_VER)),$(word 2,$(WIN_VER)),$(word 3,$(WIN_VER)),0
endif

SRC_C   := $(ENGINE) $(WEB_SRCS) $(PLATFORM_C) $(GENERATED)
SRC_CPP := $(APP) $(IMGUI) $(PLATFORM_CPP) vendor/imgui/misc/freetype/imgui_freetype.cpp
OBJS    := $(addprefix $(BUILD)/,$(SRC_CPP:.cpp=.o) $(SRC_C:.c=.o) $(PLATFORM_MM:.mm=.o)) $(WIN_RES)
HEADERS := $(wildcard app/*.h app/*.inc engine/*.h web/*.h platform/sdl/*.h platform/sdl/win32/*.h)

$(BUILD)/%.o: %.cpp $(HEADERS) | $(GENERATED)
	@mkdir -p $(dir $@)
	$(HOST_CC) -x c++ $(CXXFLAGS) -c -o $@ $<
$(BUILD)/%.o: %.c $(HEADERS) | $(GENERATED)
	@mkdir -p $(dir $@)
	$(HOST_CC) $(CFLAGS) -c -o $@ $<
$(BUILD)/%.o: %.mm $(HEADERS) | $(GENERATED)
	@mkdir -p $(dir $@)
	$(HOST_CC) -x objective-c++ -fobjc-arc $(CXXFLAGS) -c -o $@ $<

# Windows: the program's icon, its version for Explorer's Properties, and
# its manifest (win32/chiral-ftp.manifest).
gen/chiral-ftp.ico: assets/brand/icon-256.png tools/make-ico.py | gen
	$(PYTHON) tools/make-ico.py $< $@
$(BUILD)/chiral-ftp.res.o: platform/sdl/win32/chiral-ftp.rc platform/sdl/win32/chiral-ftp.manifest gen/chiral-ftp.ico
	@mkdir -p $(dir $@)
	windres -I. -DVERSION_STR=$(VERSION) -DVERSION_NUM=$(WIN_VERNUM) -o $@ $<

.PHONY: all
all: $(TARGET)
$(TARGET): $(OBJS)
	$(HOST_CC) -o $@ $^ $(DEPS)/lib/libssh2.a $(DEPS)/lib/libmbedcrypto.a \
	  $(SDL_LIBS) $(MHD_LIBS) $(SSH_LIBS) $(FT_LIBS) $(OS_LIBS) $(CXX_LIB) -pthread
	@echo "built ./$(TARGET)"

ifdef WINDOWS
# dist/chiral-ftp-<version>-windows-<x64|arm64>.exe: the program without its
# debug information, named for what the shell builds -- UCRT64 makes x64,
# CLANGARM64 arm64 -- so both can sit in one release.
WIN_ARCH := $(if $(findstring aarch64,$(shell $(HOST_CC) -dumpmachine)),arm64,x64)
WIN_DIST := dist/chiral-ftp-$(VERSION)-windows-$(WIN_ARCH).exe
.PHONY: dist
dist: $(WIN_DIST)
$(WIN_DIST): $(TARGET)
	@mkdir -p dist
	strip -o $@ $<
	@echo "built $@"

# release/: the .exe next to the consoles' files (tools/make-release.sh).
# Through bash -o igncr, since a Windows checkout may have the script with
# CRLF line endings.
.PHONY: release
release: $(WIN_DIST)
	VERSION=$(VERSION) PYTHON=$(PYTHON) bash -o igncr tools/make-release.sh windows
else
# release/: the Mac app, zipped, next to the consoles' files
# (tools/make-release.sh). On Linux the AppImage comes from tools/make-appimage.sh.
.PHONY: release
release: app
	VERSION=$(VERSION) PYTHON=$(PYTHON) ./tools/make-release.sh mac

# macOS only: dist/chiral-ftp.app, with its Homebrew libraries inside.
.PHONY: app
app: $(TARGET)
	$(PYTHON) tools/make-macos-app.py $(TARGET) $(VERSION)
endif
