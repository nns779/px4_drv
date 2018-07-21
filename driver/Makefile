KVER := $(shell uname -r)
KBUILD_DIR := /lib/modules/$(KVER)/build
INSTALL_DIR := /lib/modules/$(KVER)/misc
CURRENT_DIR := $(shell pwd)
VERBOSE := 0
DEBUG := 0
TARGET := px4_drv.ko

ccflags-y := -I$(M)/../include
ifneq ($(DEBUG),0)
ccflags-y += -DDEBUG -g
endif

obj-m := px4_drv.o
px4_drv-objs := px4.o it930x-bus.o it930x.o tc90522.o rt710.o r850_lite.o r850_channel.o ringbuffer.o

all: module

module:
	$(MAKE) -C $(KBUILD_DIR) M=$(CURRENT_DIR) KBUILD_VERBOSE=$(VERBOSE) modules

clean:
	$(MAKE) -C $(KBUILD_DIR) M=$(CURRENT_DIR) KBUILD_VERBOSE=$(VERBOSE) clean

install:
	rm -f /etc/udev/rules.d/90-px4.rules
	install -D -v -m 644 $(TARGET) $(INSTALL_DIR)/$(TARGET)
	install -D -v -m 644 ../etc/99-px4video.rules /etc/udev/rules.d/99-px4video.rules
	depmod -a $(KVER)

uninstall:
	rm -fv $(INSTALL_DIR)/$(TARGET) /etc/udev/rules.d/99-px4video.rules /etc/udev/rules.d/90-px4.rules
