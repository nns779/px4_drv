# Makefile for px4_drv

KVER := $(shell uname -r)
KDIR := /lib/modules/$(KVER)/build
PWD := $(shell pwd)
INSTALL_DIR := /lib/modules/$(KVER)/misc
VERBOSE := 0

cmd_prefix := @
ifneq ($(VERBOSE),0)
cmd_prefix :=
endif

all: modules

modules: px4_drv.ko

px4_drv.ko: FORCE revision.h
	$(cmd_prefix)$(MAKE) -C $(KDIR) M=$(PWD) KBUILD_VERBOSE=$(VERBOSE) px4_drv.ko

revision.h: FORCE
	$(cmd_prefix)rev=`git rev-list --count HEAD` 2>/dev/null; \
	rev_name=`git name-rev --name-only HEAD` 2>/dev/null; \
	commit=`git rev-list --max-count=1 HEAD` 2>/dev/null; \
	if [ ! -s $@ ] || [ \"`grep -soE '^#define REVISION_NUMBER[[:blank:]]+"[0-9]+"$$' $@ | sed -E 's/^.+"(.*)"$$/\1/g'`.`grep -soE '^#define REVISION_NAME[[:blank:]]+"[[:print:]]+"$$' $@ | sed -E 's/^.+"(.*)"$$/\1/g'`.`grep -soE '^#define COMMIT_HASH[[:blank:]]+"[0-9A-Fa-f]+"$$' $@ | sed -E 's/^.+"(.*)"$$/\1/g'`\" != \"$${rev}.$${rev_name}.$${commit}\" ]; then \
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
	$(cmd_prefix)$(MAKE) -C $(KDIR) M=$(PWD) KBUILD_VERBOSE=$(VERBOSE) clean
	$(cmd_prefix)rm -f revision.h

install:
	$(cmd_prefix)if [ `grep -e '^px4_drv' /proc/modules | wc -l` -ne 0 ]; then \
	modprobe -r px4_drv; \
	fi
	$(cmd_prefix)install -D -v -m 644 px4_drv.ko $(INSTALL_DIR)/px4_drv.ko
	$(cmd_prefix)rm -fv /etc/udev/rules.d/90-px4.rules
	$(cmd_prefix)install -D -v -m 644 ../etc/99-px4video.rules /etc/udev/rules.d/99-px4video.rules
	$(cmd_prefix)depmod -a $(KVER)
	$(cmd_prefix)modprobe px4_drv

uninstall:
	$(cmd_prefix)if [ `grep -e '^px4_drv' /proc/modules | wc -l` -ne 0 ]; then \
	modprobe -r px4_drv; \
	fi
	$(cmd_prefix)rm -fv $(INSTALL_DIR)/px4_drv.ko
	$(cmd_prefix)if [ `find /lib/modules/ -name px4_drv.ko | wc -l` -eq 0 ]; then \
	rm -fv /etc/udev/rules.d/90-px4.rules /etc/udev/rules.d/99-px4video.rules; \
	fi
	$(cmd_prefix)depmod -a $(KVER)

FORCE:

.PHONY: clean install uninstall FORCE
