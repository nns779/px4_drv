CXX ?= c++
PKG_CONFIG ?= pkg-config
PCSC_CFLAGS ?= $(shell $(PKG_CONFIG) --cflags libpcsclite)
USBDROPDIR ?= $(shell $(PKG_CONFIG) --variable=usbdropdir libpcsclite)
BUILD ?= build
ROOT := ..
SHARED := $(ROOT)/winusb/src/DriverHost_PX4
CPPFLAGS += -I$(ROOT)/include -I$(ROOT)/driver -I$(SHARED) $(PCSC_CFLAGS)
CPPFLAGS += -I.
CXXFLAGS ?= -O2 -g
CXXFLAGS += -std=c++17 -Wall -Wextra -Wpedantic -pthread
SOURCES := ifdhandler.cpp linux_card_device.cpp $(SHARED)/smart_card.cpp
HEADERS := linux_card_device.hpp $(SHARED)/card_device.hpp $(SHARED)/smart_card.hpp $(ROOT)/include/px4_card_ioctl.h

all: $(BUILD)/libifd-px4.so

$(BUILD):
	mkdir -p $@

$(BUILD)/libifd-px4.so: $(SOURCES) $(HEADERS) exports.map | $(BUILD)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -fPIC -shared $(SOURCES) $(LDFLAGS) -Wl,--version-script=exports.map -o $@

$(BUILD)/smart_card_state_test: $(ROOT)/winusb/tests/smart_card_state_test.cpp $(SHARED)/smart_card.cpp $(HEADERS) | $(BUILD)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $< $(SHARED)/smart_card.cpp $(LDFLAGS) -o $@

$(BUILD)/ifdhandler_test: tests/ifdhandler_test.cpp $(SOURCES) $(HEADERS) | $(BUILD)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -UNDEBUG $< $(SOURCES) $(LDFLAGS) -Wl,--wrap=open,--wrap=close,--wrap=ioctl -rdynamic -ldl -o $@

check: $(BUILD)/smart_card_state_test $(BUILD)/ifdhandler_test
	$(BUILD)/smart_card_state_test
	$(BUILD)/ifdhandler_test
	python3 tests/check_card_guids.py

$(BUILD)/libifd-px4-test.so: tests/ifdhandler_test.cpp $(SOURCES) $(HEADERS) exports.map | $(BUILD)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -UNDEBUG -DPX4_IFD_FAKE_MODULE -fPIC -shared $< $(SOURCES) $(LDFLAGS) -Wl,--wrap=open,--wrap=close,--wrap=ioctl,--version-script=exports.map -o $@

$(BUILD)/pcsc_integration_test: tests/pcsc_integration_test.cpp | $(BUILD)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -UNDEBUG $< $(LDFLAGS) $(shell $(PKG_CONFIG) --libs libpcsclite) -o $@

integration-build: $(BUILD)/libifd-px4-test.so $(BUILD)/pcsc_integration_test

install: all
	test -n "$(USBDROPDIR)"
	install -D -m 0755 $(BUILD)/libifd-px4.so $(DESTDIR)$(USBDROPDIR)/ifd-px4.bundle/Contents/Linux/libifd-px4.so
	install -D -m 0644 Info.plist $(DESTDIR)$(USBDROPDIR)/ifd-px4.bundle/Contents/Info.plist

.PHONY: all check install integration-build
