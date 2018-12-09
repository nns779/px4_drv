KVER := $(shell uname -r)
KBUILD_DIR := /lib/modules/$(KVER)/build
INSTALL_DIR := /lib/modules/$(KVER)/misc
CURRENT_DIR := $(shell pwd)
VERBOSE := 0
DEBUG := 0
TARGET := px4_drv.ko

drv-MAX_DEVICE := 0

cmd_prefix := @

ccflags-y := -I$(M)/../include
ifneq ($(DEBUG),0)
ccflags-y += -DDEBUG -g
endif
ifneq ($(VERBOSE),0)
cmd_prefix :=
endif
ifneq ($(drv-MAX_DEVICE),0)
ccflags-y += -DMAX_DEVICE=$(drv-MAX_DEVICE)
endif

obj-m := px4_drv.o
px4_drv-objs := px4.o it930x-bus.o it930x.o tc90522.o rt710.o r850_lite.o r850_channel.o ringbuffer.o

all: $(TARGET)

$(TARGET): FORCE revision.h
	$(cmd_prefix)$(MAKE) -C $(KBUILD_DIR) M=$(CURRENT_DIR) KBUILD_VERBOSE=$(VERBOSE) modules

revision.h: FORCE
	$(cmd_prefix)rev=`git rev-list --count HEAD` 2>/dev/null; \
	rev_name=`git name-rev --name-only HEAD` 2>/dev/null; \
	commit=`git rev-list --max-count=1 HEAD` 2>/dev/null; \
	if [ ! -s $@ ] || [ \"`grep -soP '(?<=#define REVISION_NUMBER\\s\\")[0-9]+(?=\\")' $@`.`grep -soP '(?<=#define REVISION_NAME\\s\\")[[:print:]]+(?=\\")' $@`.`grep -soP '(?<=#define COMMIT_HASH\\s\\")[0-9A-Fa-f]+(?=\\")' $@`\" != \"$${rev}.$${rev_name}.$${commit}\" ]; then \
	echo "// revision.h" > $@; \
	echo "" >> $@; \
	echo "#ifndef __REVISION_H__" >> $@; \
	echo "#define __REVISION_H__" >> $@; \
	echo "" >> $@; \
	if [ -n "$${rev}" ]; then \
	echo "#define REVISION_NUMBER	\"$${rev}\"" >> $@; \
	fi; \
	if [ -n "$${rev_name}" ]; then \
	echo "#define REVISION_NAME	\"$${rev_name}\"" >> $@; \
	fi; \
	if [ -n "$${commit}" ]; then \
	echo "#define COMMIT_HASH	\"$${commit}\"" >> $@; \
	fi; \
	echo "" >> $@; \
	echo "#endif" >> $@; \
	echo "'revision.h' was updated."; \
	fi

clean:
	$(cmd_prefix)$(MAKE) -C $(KBUILD_DIR) M=$(CURRENT_DIR) KBUILD_VERBOSE=$(VERBOSE) clean
	$(cmd_prefix)rm -f revision.h

install:
	$(cmd_prefix)install -D -v -m 644 $(TARGET) $(INSTALL_DIR)/$(TARGET)
	$(cmd_prefix)rm -fv /etc/udev/rules.d/90-px4.rules
	$(cmd_prefix)install -D -v -m 644 ../etc/99-px4video.rules /etc/udev/rules.d/99-px4video.rules
	$(cmd_prefix)depmod -a $(KVER)

uninstall:
	$(cmd_prefix)rm -fv $(INSTALL_DIR)/$(TARGET)
	$(cmd_prefix)if [ `find /lib/modules/ -name px4_drv.ko | wc -l` -eq 0 ]; then \
	rm -fv /etc/udev/rules.d/90-px4.rules /etc/udev/rules.d/99-px4video.rules; \
	fi
	$(cmd_prefix)depmod -a $(KVER)

FORCE:

.PHONY: clean install uninstall FORCE
