KVER := $(shell uname -r)
KBUILD_DIR := /lib/modules/$(KVER)/build
INSTALL_DIR := /lib/modules/$(KVER)/misc
CURRENT_DIR := $(shell pwd)
VERBOSE := 0
DEBUG := 0
TARGET := px4_drv.ko

drv-MAX_DEVICE := 0

prefix := @

ccflags-y := -I$(M)/../include
ifneq ($(DEBUG),0)
ccflags-y += -DDEBUG -g
endif
ifneq ($(VERBOSE),0)
prefix :=
endif
ifneq ($(drv-MAX_DEVICE),0)
ccflags-y += -DMAX_DEVICE=$(drv-MAX_DEVICE)
endif

obj-m := px4_drv.o
px4_drv-objs := px4.o it930x-bus.o it930x.o tc90522.o rt710.o r850_lite.o r850_channel.o ringbuffer.o

all: module

module: revision.h
	$(MAKE) -C $(KBUILD_DIR) M=$(CURRENT_DIR) KBUILD_VERBOSE=$(VERBOSE) modules

revision.h: FORCE
	$(prefix)rev=`git rev-list --count HEAD` 2>/dev/null; \
	commit=`git rev-list --max-count=1 HEAD` 2>/dev/null; \
	if [ ! -s $@ ] || [ \"`grep -soP '(?<=#define REVISION_NUMBER\\s\\")[0-9]+(?=\\")' $@`.`grep -soP '(?<=#define COMMIT_HASH\\s\\")[0-9A-Fa-f]+(?=\\")' $@`\" != \"$${rev}.$${commit}\" ]; then \
	echo "// revision.h" > $@; \
	echo "" >> $@; \
	echo "#ifndef __REVISION_H__" >> $@; \
	echo "#define __REVISION_H__" >> $@; \
	echo "" >> $@; \
	if [ -n "$${rev}" ]; then \
	echo "#define REVISION_NUMBER	\"$${rev}\"" >> $@; \
	fi; \
	if [ -n "$${commit}" ]; then \
	echo "#define COMMIT_HASH	\"$${commit}\"" >> $@; \
	fi; \
	echo "" >> $@; \
	echo "#endif" >> $@; \
	echo "'revision.h' was updated."; \
	fi

clean:
	$(MAKE) -C $(KBUILD_DIR) M=$(CURRENT_DIR) KBUILD_VERBOSE=$(VERBOSE) clean
	rm -f revision.h

install:
	@rm -f /etc/udev/rules.d/90-px4.rules
	install -D -v -m 644 $(TARGET) $(INSTALL_DIR)/$(TARGET)
	install -D -v -m 644 ../etc/99-px4video.rules /etc/udev/rules.d/99-px4video.rules
	depmod -a $(KVER)

uninstall:
	rm -fv $(INSTALL_DIR)/$(TARGET) /etc/udev/rules.d/99-px4video.rules /etc/udev/rules.d/90-px4.rules

FORCE:
