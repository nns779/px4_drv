# Makefile for px4_drv
KMODULES_DIRS := $(shell ls -1 "/lib/modules")
KMODULES_COUNT := $(shell ls -1 "/lib/modules" | wc -l)
HOST_KVER := $(shell uname -r)

ifeq ($(KMODULES_COUNT),1)
    KVER := $(KMODULES_DIRS)
else ifeq ($(KMODULES_COUNT),2)
    ifeq ($(HOST_KVER),)
        $(error HOST_KVER is not set while multiple kernel versions exist)
    endif
    KVER := $(shell ls -1 "/lib/modules" | grep -v "$(HOST_KVER)" | head -n1)
    ifeq ($(KVER),)
        $(error Failed to find alternative kernel version)
    endif
else
    $(error Unexpected number of kernel versions in /lib/modules: $(KMODULES_COUNT))
endif
KDIR := /lib/modules/$(KVER)/build
PWD := $(shell pwd)
INSTALL_DIR := /lib/modules/$(KVER)/misc

all: modules

modules: px4_drv.ko

px4_drv.ko: FORCE revision.h
	$(MAKE) -C $(KDIR) M=$(PWD) KBUILD_VERBOSE=$(VERBOSE) px4_drv.ko

deb: all
	dpkg-buildpackage -b -rfakeroot -us -uc

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) KBUILD_VERBOSE=$(VERBOSE) clean
	rm -f revision.h

install:
	if [ `grep -e '^px4_drv' /proc/modules | wc -l` -ne 0 ]; then \
	modprobe -r px4_drv; \
	fi
	install -D -v -m 644 px4_drv.ko $(INSTALL_DIR)/px4_drv.ko
	rm -fv /etc/udev/rules.d/90-px4.rules
	install -D -v -m 644 ./99-px4video.rules /etc/udev/rules.d/99-px4video.rules
	depmod -a $(KVER)
	modprobe px4_drv

uninstall:
	if [ `grep -e '^px4_drv' /proc/modules | wc -l` -ne 0 ]; then \
	modprobe -r px4_drv; \
	fi
	rm -fv $(INSTALL_DIR)/px4_drv.ko
	if [ `find /lib/modules/ -name px4_drv.ko | wc -l` -eq 0 ]; then \
	rm -fv /etc/udev/rules.d/90-px4.rules /etc/udev/rules.d/99-px4video.rules; \
	fi
	depmod -a $(KVER)

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

.PHONY: deb clean install uninstall FORCE
