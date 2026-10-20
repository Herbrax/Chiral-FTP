# This computer: an SDL window. Linux, macOS (including Apple silicon) and
# Windows under MSYS2. Needs SDL2, libmicrohttpd and pkg-config, plus
# libssh2 and mbedTLS -- tools/build-sftp-deps.sh builds those locally.
.DEFAULT_GOAL := all

TARGET  := chiral-ftp
BUILD   := .build/sdl
HOST_CC ?= cc
DEPS    := .deps/host

PLATFORM_CPP := platform/sdl/backend.cpp
PLATFORM_C   := platform/sdl/system.c
GENERATED    := $(FONTS) $(WEB_GEN) $(GEN_I18N)

MHD_CFLAGS := $(shell pkg-config libmicrohttpd --cflags)
MHD_LIBS   := $(shell pkg-config libmicrohttpd --libs)
SDL_CFLAGS := $(shell sdl2-config --cflags)
SDL_LIBS   := $(shell sdl2-config --libs)

DEFINES  := -DVERSION_TAG=\"$(VERSION)-sdl\" -DIMGUI_USER_CONFIG=\"imgui_config.h\"
INCLUDE  := -Iapp -Iengine -Iweb -Ivendor/imgui -Igen -Iplatform/sdl -I$(DEPS)/include
CFLAGS   := -O2 -g -Wall -pthread $(DEFINES) $(INCLUDE) $(MHD_CFLAGS)
CXXFLAGS := -std=c++17 -O2 -g -Wall $(DEFINES) $(INCLUDE) $(MHD_CFLAGS) $(SDL_CFLAGS)

SRC_C   := $(ENGINE) $(WEB_SRCS) $(PLATFORM_C) $(GENERATED)
SRC_CPP := $(APP) $(IMGUI) $(PLATFORM_CPP)
OBJS    := $(addprefix $(BUILD)/,$(SRC_CPP:.cpp=.o) $(SRC_C:.c=.o))
HEADERS := $(wildcard app/*.h engine/*.h web/*.h platform/sdl/*.h)

$(BUILD)/%.o: %.cpp $(HEADERS) | $(GENERATED)
	@mkdir -p $(dir $@)
	$(HOST_CC) -x c++ $(CXXFLAGS) -c -o $@ $<
$(BUILD)/%.o: %.c $(HEADERS) | $(GENERATED)
	@mkdir -p $(dir $@)
	$(HOST_CC) $(CFLAGS) -c -o $@ $<

.PHONY: all
all: $(TARGET)
$(TARGET): $(OBJS)
	$(HOST_CC) -o $@ $^ $(DEPS)/lib/libssh2.a $(DEPS)/lib/libmbedcrypto.a \
	  $(SDL_LIBS) $(MHD_LIBS) -lc++ -pthread
	@echo "built ./$(TARGET)"
