# Kbuild for px4_drv

DEBUG := 0

PX4_USB_MAX_DEVICE := 0
PXMLT5_USB_MAX_DEVICE := 0
PXMLT8_USB_MAX_DEVICE := 0
ISDB2056_USB_MAX_DEVICE := 0
ISDB6014_4TS_USB_MAX_DEVICE := 0
PXM1UR_USB_MAX_DEVICE := 0
PXS1UR_USB_MAX_DEVICE := 0
PSB_DEBUG := 0
ITEDTV_BUS_USE_WORKQUEUE := 0

ccflags-y := -I$(M)/../include

ifneq ($(DEBUG),0)
ccflags-y += -DDEBUG -g
endif

ifneq ($(PX4_USB_MAX_DEVICE),0)
ccflags-y += -DPX4_USB_MAX_DEVICE=$(PX4_USB_MAX_DEVICE)
endif
ifneq ($(PXMLT5_USB_MAX_DEVICE),0)
ccflags-y += -DPXMLT5_USB_MAX_DEVICE=$(PXMLT5_USB_MAX_DEVICE)
endif
ifneq ($(PXMLT8_USB_MAX_DEVICE),0)
ccflags-y += -DPXMLT8_USB_MAX_DEVICE=$(PXMLT8_USB_MAX_DEVICE)
endif
ifneq ($(ISDB2056_USB_MAX_DEVICE),0)
ccflags-y += -DISDB2056_USB_MAX_DEVICE=$(ISDB2056_USB_MAX_DEVICE)
endif
ifneq ($(ISDB6014_4TS_USB_MAX_DEVICE),0)
ccflags-y += -DISDB6014_4TS_USB_MAX_DEVICE=$(ISDB6014_4TS_USB_MAX_DEVICE)
endif
ifneq ($(PXM1UR_USB_MAX_DEVICE),0)
ccflags-y += -DPXM1UR_USB_MAX_DEVICE=$(PXM1UR_USB_MAX_DEVICE)
endif
ifneq ($(PXS1UR_USB_MAX_DEVICE),0)
ccflags-y += -DPXS1UR_USB_MAX_DEVICE=$(PXS1UR_USB_MAX_DEVICE)
endif
ifneq ($(PSB_DEBUG),0)
ccflags-y += -DPSB_DEBUG
endif
ifneq ($(ITEDTV_BUS_USE_WORKQUEUE),0)
ccflags-y += -DITEDTV_BUS_USE_WORKQUEUE
endif

obj-m := px4_drv.o
px4_drv-y := driver_module.o ptx_chrdev.o px4_usb.o px4_usb_params.o px4_device.o px4_device_params.o px4_mldev.o pxmlt_device.o isdb2056_device.o it930x.o itedtv_bus.o tc90522.o r850.o rt710.o cxd2856er.o cxd2858er.o ringbuffer.o s1ur_device.o m1ur_device.o
