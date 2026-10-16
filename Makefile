# SPDX-License-Identifier: GPL-2.0
#
# avmcv710 - kernel capture driver for the AVerMedia ExtremeCap U3 (CV710)
#
# In-tree (Linux media tree):
#   drivers/media/usb/avmcv710/Makefile
#
# Out-of-tree build against the running kernel:
#   make -C /lib/modules/$(uname -r)/build M=$(pwd) modules
#   (BSD make will not work; this requires GNU make.)

avmcv710-y := avmcv710-hw.o avmcv710-parser.o avmcv710-usb.o avmcv710-video.o avmcv710-cvx8897.o avmcv710-audio.o avmcv710-status.o

# In-tree: built according to Kconfig.  Out-of-tree (no CONFIG symbol):
# build as a standalone module.
ifdef CONFIG_VIDEO_AVMCV710
obj-$(CONFIG_VIDEO_AVMCV710) += avmcv710.o
else
obj-m += avmcv710.o
endif

ccflags-y += -Wframe-larger-than=2048

# Convenience wrapper for out-of-tree builds:
KDIR ?= $(shell if [ -d "/lib/modules/$$(uname -r)/build" ]; then echo "/lib/modules/$$(uname -r)/build"; else ls -d /lib/modules/*/build 2>/dev/null | sort -V | tail -n 1; fi)
LLVM ?= $(shell grep -q '^CONFIG_CC_IS_CLANG=y' "$(KDIR)/.config" 2>/dev/null && echo 1)
KBUILD_ARGS := $(if $(LLVM),LLVM=$(LLVM))

PKG_NAME := avmcv710
PKG_VER  := 0.1.0
DKMS_DIR := /usr/src/$(PKG_NAME)-$(PKG_VER)

.PHONY: modules clean install dkms-install dkms-remove
modules:
	$(MAKE) -C $(KDIR) M=$(CURDIR) $(KBUILD_ARGS) modules

clean:
	$(MAKE) -C $(KDIR) M=$(CURDIR) $(KBUILD_ARGS) clean

install: modules
	$(MAKE) -C $(KDIR) M=$(CURDIR) $(KBUILD_ARGS) modules_install

dkms-install:
	install -d $(DKMS_DIR)
	install -m 0644 Makefile Kconfig dkms.conf *.c *.h $(DKMS_DIR)/
	dkms remove -m $(PKG_NAME) -v $(PKG_VER) --all 2>/dev/null || true
	dkms add -m $(PKG_NAME) -v $(PKG_VER)
	dkms build -m $(PKG_NAME) -v $(PKG_VER)
	dkms install -m $(PKG_NAME) -v $(PKG_VER) --force

dkms-remove:
	dkms remove -m $(PKG_NAME) -v $(PKG_VER) --all 2>/dev/null || true
	rm -rf $(DKMS_DIR)