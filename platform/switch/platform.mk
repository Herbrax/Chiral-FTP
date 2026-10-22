# The Switch: devkitPro / devkitA64, libnx, output is a .nro.
#
#   dkp-pacman -S switch-dev switch-libssh2 switch-mbedtls switch-zlib
#   export DEVKITPRO=/opt/devkitpro
#
# libmicrohttpd has no devkitPro package; tools/build-switch-deps.sh
# cross-compiles it into .deps/switch on the first build. WEB=0 leaves the
# web app out and drops that dependency.

ifeq ($(strip $(DEVKITPRO)),)
$(error Set DEVKITPRO in your environment, e.g. export DEVKITPRO=/opt/devkitpro)
endif
include $(DEVKITPRO)/libnx/switch_rules
.DEFAULT_GOAL := all

APP_TITLE   := Chiral-FTP
APP_AUTHOR  := Herbrax
APP_VERSION := $(VERSION)
TARGET      := chiral-ftp
BUILD       := .build
ICON        := assets/brand/icon.jpg

WEB ?= 1
ifeq ($(WEB),1)
  MHD_LIB := -lmicrohttpd
  MHD_DEP := .deps/switch/lib/libmicrohttpd.a
  WEB_C   := $(notdir $(WEB_SRCS))
else
  MHD_LIB :=
  MHD_DEP :=
  WEB_C   := $(notdir $(WEB_STUB))
  WEB_GEN :=
endif

PLATFORM_CPP := platform/switch/backend.cpp
PLATFORM_C   := platform/switch/system.c platform/switch/init.c platform/switch/net.c platform/switch/usb_drives.c

GENERATED := $(FONTS) $(WEB_GEN) $(GEN_I18N)

ARCH    := -march=armv8-a+crc+crypto -mtune=cortex-a57 -mtp=soft -fPIE
DEFINES := -D__SWITCH__ -DVERSION_TAG=\"$(VERSION)\" \
           -DIMGUI_USER_CONFIG=\"imgui_config.h\"
CFLAGS   := -g -Wall -O2 -ffunction-sections -D_GNU_SOURCE $(ARCH) $(DEFINES) $(INCLUDE)
CXXFLAGS := $(CFLAGS) -std=gnu++17 -fno-rtti
ASFLAGS  := -g $(ARCH)
LDFLAGS  := -specs=$(DEVKITPRO)/libnx/switch.specs -g $(ARCH) -Wl,-Map,$(notdir $*.map)
# USB drives: libusbhsfs as Goldleaf builds it (tools/build-switch-usbhsfs.sh),
# with the NTFS-3G and lwext4 portlibs (dkp-pacman -S switch-ntfs-3g switch-lwext4).
USB_LIB  := -lusbhsfs -lntfs-3g -llwext4
USB_DEP  := .deps/switch/lib/libusbhsfs.a
LIBS     := -lssh2 $(MHD_LIB) $(USB_LIB) -lmbedtls -lmbedx509 -lmbedcrypto -lz -lnx
LIBDIRS  := $(PORTLIBS) $(LIBNX) $(CURDIR)/.deps/switch

.deps/switch/lib/libmicrohttpd.a:
	./tools/build-switch-deps.sh
.deps/switch/lib/libusbhsfs.a:
	./tools/build-switch-usbhsfs.sh
deps: .deps/switch/lib/libmicrohttpd.a .deps/switch/lib/libusbhsfs.a

ifneq ($(BUILD),$(notdir $(CURDIR)))

export OUTPUT  := $(CURDIR)/$(TARGET)
export TOPDIR  := $(CURDIR)
export DEPSDIR := $(CURDIR)/$(BUILD)
export VPATH   := $(foreach d,engine web web/stub app vendor/imgui platform/switch gen,$(CURDIR)/$(d))

# Named, not globbed: make expands wildcards while it parses, which is
# before the generators have run, and a clean tree would silently drop
# every generated object from the link.
CFILES   := $(notdir $(ENGINE)) $(WEB_C) $(notdir $(PLATFORM_C)) $(notdir $(GENERATED))
CPPFILES := $(notdir $(APP)) $(notdir $(IMGUI)) $(notdir $(PLATFORM_CPP))

export LD := $(CXX)
export OFILES := $(CPPFILES:.cpp=.o) $(CFILES:.c=.o)
export INCLUDE := $(foreach d,app engine web vendor/imgui gen platform/switch,-I$(CURDIR)/$(d)) \
                  $(foreach d,$(LIBDIRS),-I$(d)/include) -I$(CURDIR)/$(BUILD)
export LIBPATHS := $(foreach d,$(LIBDIRS),-L$(d)/lib)
export APP_ICON := $(if $(wildcard $(ICON)),$(CURDIR)/$(ICON),$(LIBNX)/default_icon.jpg)
export NROFLAGS += --icon=$(APP_ICON) --nacp=$(CURDIR)/$(TARGET).nacp

# Phony on purpose: it names a directory, so without this make calls it up
# to date the moment the directory exists and never recurses.
.PHONY: $(BUILD) all dist release deps

all: dist

$(BUILD): $(GENERATED) $(MHD_DEP) $(USB_DEP)
	@mkdir -p $@
	@$(MAKE) --no-print-directory -C $(BUILD) -f $(CURDIR)/Makefile PLATFORM=switch

nro: $(BUILD)

dist: nro
	@mkdir -p dist/chiral-ftp
	@cp $(TARGET).nro dist/chiral-ftp/
	@cp LICENSE dist/chiral-ftp/
	@echo "built dist/chiral-ftp/chiral-ftp.nro"

release: dist
	VERSION=$(VERSION) PYTHON=$(PYTHON) ./tools/make-release.sh switch

else

DEPENDS := $(OFILES:.o=.d)
all: $(OUTPUT).nro
$(OUTPUT).nro: $(OUTPUT).elf $(OUTPUT).nacp
$(OUTPUT).elf: $(OFILES)
-include $(DEPENDS)

endif
