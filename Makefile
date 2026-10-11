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

avmcv710-y := avmcv710-hw.o avmcv710-parser.o avmcv710-usb.o avmcv710-video.o avmcv710-cvx8897.o

# In-tree: built according to Kconfig.  Out-of-tree (no CONFIG symbol):
# build as a standalone module.
ifdef CONFIG_VIDEO_AVMCV710
obj-$(CONFIG_VIDEO_AVMCV710) += avmcv710.o
else
obj-m += avmcv710.o
endif

ccflags-y += -Wframe-larger-than=2048

# Convenience wrapper for out-of-tree builds:
#   make KDIR=/lib/modules/$(uname -r)/build
KDIR ?= /lib/modules/$(shell uname -r)/build

.PHONY: modules clean install
modules:
	$(MAKE) -C $(KDIR) M=$(CURDIR) modules

clean:
	$(MAKE) -C $(KDIR) M=$(CURDIR) clean

install: modules
	$(MAKE) -C $(KDIR) M=$(CURDIR) modules_install