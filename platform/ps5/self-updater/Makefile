# chiral-ftp's self-updater.elf: the boilerplate's self-update helper
# (examples/self-update-helper, GPL-3.0-or-later) made chiral-ftp's, built as a
# payload for the console's loader. tools/build-ps5.sh runs it:
#
#   make -C platform/ps5/self-updater PS5_PAYLOAD_SDK=<sdk> ROOT=<repo> OUTPUT=<elf>

ifdef PS5_PAYLOAD_SDK
include $(PS5_PAYLOAD_SDK)/toolchain/prospero.mk
else
include /opt/ps5-payload-sdk/toolchain/prospero.mk
endif

ROOT ?= ../../..
OUTPUT ?= $(ROOT)/.build/ps5-app/self-updater/self-updater.elf
WORK := $(dir $(OUTPUT))
CXXFLAGS := -std=c++20 -O2 -Wall -Wextra -Werror -fno-exceptions -fno-rtti -I. -I$(ROOT)/vendor
MINIZ_CFLAGS := -std=c11 -O2 -w
LDFLAGS := -Wl,--build-id=none

CXX_SOURCES := main.cpp updater.cpp archive.cpp files.cpp
MINIZ_SOURCES := $(addprefix $(ROOT)/vendor/miniz/,miniz.c miniz_tinfl.c miniz_tdef.c miniz_zip.c)
OBJECTS := $(addprefix $(WORK),$(CXX_SOURCES:.cpp=.o) $(notdir $(MINIZ_SOURCES:.c=.o)))
vpath %.c $(ROOT)/vendor/miniz

.PHONY: all clean

all: $(OUTPUT)

$(WORK)%.o: %.cpp $(wildcard *.hpp *.h)
	@mkdir -p -- $(@D)
	$(CXX) $(CXXFLAGS) -c -o $@ $<

$(WORK)miniz%.o: miniz%.c
	@mkdir -p -- $(@D)
	$(CC) $(MINIZ_CFLAGS) -c -o $@ $<

$(OUTPUT): $(OBJECTS)
	$(CXX) $(LDFLAGS) -o $@ $^

clean:
	rm -rf -- $(WORK)
