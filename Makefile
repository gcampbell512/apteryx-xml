# Makefile for Apteryx XML based schema utiltiies
#
# Unit Tests (make test FILTER): e.g make test LUA
# Requires GLib, Lua and libXML. CUnit for Unit Testing.
# sudo apt-get install libglib2.0-dev liblua5.2-dev libxml2-dev libcunit1-dev
#
# TEST_WRAPPER="G_SLICE=always-malloc valgrind --leak-check=full" make test
# TEST_WRAPPER="gdb" make test
#

ifneq ($(V),1)
	Q=@
endif

DESTDIR?=./
PREFIX?=/usr/
LIBDIR?=lib
CC:=$(CROSS_COMPILE)gcc
LD:=$(CROSS_COMPILE)ld
PKG_CONFIG ?= pkg-config
APTERYX_PATH ?=

ABI_VERSION=1.4
CFLAGS := $(CFLAGS) -g -O2
EXTRA_CFLAGS += -Wall -Wno-comment -std=c99 -D_GNU_SOURCE -fPIC
EXTRA_CFLAGS += -I. $(shell $(PKG_CONFIG) --cflags glib-2.0)
EXTRA_LDFLAGS += $(shell $(PKG_CONFIG) --libs glib-2.0) -lpthread
ifndef APTERYX_PATH
EXTRA_CFLAGS += $(shell $(PKG_CONFIG) --cflags apteryx)
EXTRA_LDFLAGS += $(shell $(PKG_CONFIG) --libs apteryx)
else
EXTRA_CFLAGS += -I$(APTERYX_PATH)
EXTRA_LDFLAGS += -L$(APTERYX_PATH) -lapteryx
endif
LUAVERSION := $(shell $(PKG_CONFIG) --exists lua && echo lua ||\
	($(PKG_CONFIG) --exists lua5.4 && echo lua5.4 ||\
	($(PKG_CONFIG) --exists lua5.3 && echo lua5.3 ||\
	($(PKG_CONFIG) --exists lua5.2 && echo lua5.2 ||\
	echo none))))

ifneq ($(LUAVERSION),none)
EXTRA_CFLAGS += -DHAVE_LUA $(shell $(PKG_CONFIG) --cflags $(LUAVERSION))
EXTRA_LDFLAGS += $(shell $(PKG_CONFIG) --libs $(LUAVERSION)) -ldl
endif
EXTRA_CFLAGS += -DHAVE_LIBXML2 $(shell $(PKG_CONFIG) --cflags libxml-2.0 jansson)
EXTRA_LDFLAGS += $(shell $(PKG_CONFIG) --libs libxml-2.0 jansson)

all: libapteryx-xml.so libapteryx-schema.so apteryx/xml.so

libapteryx-schema.so.$(ABI_VERSION): schema.o  sch_xpath.o sch_conditions.o
	@echo "Creating library "$@""
	$(Q)$(CC) -shared $(LDFLAGS) -o $@ $^ $(EXTRA_LDFLAGS) -Wl,-soname,$@

lib%.so: lib%.so.$(ABI_VERSION)
	@ln -s -f $< $@

libapteryx-xml.so.$(ABI_VERSION): libapteryx-schema.so
libapteryx-xml.so.$(ABI_VERSION): lua.o
	@echo "Creating library "$@""
	$(Q)$(CC) -shared $(LDFLAGS) -o $@ $< $(EXTRA_LDFLAGS) -L. -lapteryx-schema -Wl,-soname,$@

apteryx/xml.so: libapteryx-xml.so
	$(Q)mkdir -p apteryx
	$(Q)cp libapteryx-xml.so apteryx/xml.so

%.o: %.c
	@echo "Compiling "$<""
	$(Q)$(CC) $(CFLAGS) $(EXTRA_CFLAGS) -c $< -o $@

unittest: libapteryx-xml.so libapteryx-schema.so apteryx/xml.so
unittest: test.c
	@echo "Building $@"
	$(Q)$(CC) $(CFLAGS) $(EXTRA_CFLAGS) -o $@ $^ $(EXTRA_LDFLAGS) -L. -lapteryx-xml -lapteryx-schema -lcunit

apteryxd = \
	if test -e /tmp/apteryxd.pid; then \
		kill -TERM `cat /tmp/apteryxd.pid` && sleep 0.1; \
	fi; \
	rm -f /tmp/apteryxd.pid; \
	rm -f /tmp/apteryxd.run; \
	LD_LIBRARY_PATH=$(LD_LIBRARY_PATH):./:$(APTERYX_PATH) $(APTERYX_PATH)/apteryxd -b -p /tmp/apteryxd.pid -r /tmp/apteryxd.run && sleep 0.1; \
	LD_LIBRARY_PATH=$(LD_LIBRARY_PATH):./:$(APTERYX_PATH) $(TEST_WRAPPER) ./$(1); \
	kill -TERM `cat /tmp/apteryxd.pid`;

ifeq (test,$(firstword $(MAKECMDGOALS)))
TEST_ARGS := $(wordlist 2,$(words $(MAKECMDGOALS)),$(MAKECMDGOALS))
$(eval $(TEST_ARGS):;@:)
endif

test: unittest
	@echo "Running XML unit tests"
	$(Q)$(call apteryxd,unittest -u$(TEST_ARGS))
	@echo "Tests have been run!"

install: all
	$(Q)install -d $(DESTDIR)/etc/apteryx/schema
	$(Q)install -D -m 0644 apteryx.xsd $(DESTDIR)/etc/apteryx/schema/
	$(Q)install -d $(DESTDIR)/$(PREFIX)/$(LIBDIR)
	$(Q)install -D libapteryx-xml.so.$(ABI_VERSION) $(DESTDIR)/$(PREFIX)/$(LIBDIR)/
	$(Q)install -D libapteryx-schema.so.$(ABI_VERSION) $(DESTDIR)/$(PREFIX)/$(LIBDIR)/
	$(Q)ln -s libapteryx-xml.so.$(ABI_VERSION) $(DESTDIR)/$(PREFIX)/$(LIBDIR)/libapteryx-xml.so
	$(Q)ln -s libapteryx-schema.so.$(ABI_VERSION) $(DESTDIR)/$(PREFIX)/$(LIBDIR)/libapteryx-schema.so
	$(Q)install -d $(DESTDIR)/$(PREFIX)/include
	$(Q)install -D apteryx-xml.h $(DESTDIR)/$(PREFIX)/include/
	$(Q)install -d $(DESTDIR)/$(PREFIX)/lib/pkgconfig/
	$(Q)install -D apteryx-xml.pc $(DESTDIR)/$(PREFIX)/lib/pkgconfig/

clean:
	@echo "Cleaning..."
	@rm -fr libapteryx-schema.so* libapteryx-xml.so* apteryx/xml.so unittest *.o

.PHONY: all clean
