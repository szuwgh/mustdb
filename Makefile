PROJECT := mustdb
TOPDIR := $(CURDIR)
BUILD_DIR ?= $(TOPDIR)/build/mustdb
MUSTDB_INCLUDE := $(TOPDIR)/src/include

CC = cc
AR = ar
CFLAGS = -O2 -fPIC
CPPFLAGS = -I/home/unvdb/cproject/mustdb/build/include -I/home/unvdb/cproject/mustdb/build/include/libv
LDFLAGS = -L/home/unvdb/cproject/mustdb/build/lib
LIBS = -lv
LIBV_DIR = libv
LIBV_INCLUDEDIR = libv/src/include
LIBV_LIBDIR = libv
LIBV_LIBS = -lv
PREFIX = /home/unvdb/cproject/mustdb/build
LIBDIR = /home/unvdb/cproject/mustdb/build/lib
INCLUDEDIR = /home/unvdb/cproject/mustdb/build/include
INSTALL ?= install

LIBV_ABS := $(abspath $(LIBV_DIR))
LIBV_INCLUDEDIR_ABS := $(abspath $(LIBV_INCLUDEDIR))
LIBV_LIBDIR_ABS := $(abspath $(LIBV_LIBDIR))
SRC_CPPFLAGS := $(CPPFLAGS)
SRC_LDFLAGS := $(LDFLAGS) -L$(LIBV_LIBDIR_ABS)
LDLIBS := $(LIBV_LIBS) $(filter-out $(LIBV_LIBS),$(LIBS))

OBJS := $(shell $(MAKE) --no-print-directory -s -C src print-objs TOPDIR=$(TOPDIR) BUILD_DIR=$(BUILD_DIR) MUSTDB_INCLUDE=$(MUSTDB_INCLUDE) LIBV_INCLUDEDIR=$(LIBV_INCLUDEDIR_ABS))
STATIC_LIB := libmustdb.a
SHARED_LIB := libmustdb.so

.NOTPARALLEL:
.PHONY: all clean distclean install uninstall test libv-local src-build compat-libs

all: libv-local src-build $(STATIC_LIB) $(SHARED_LIB) compat-libs

libv-local:
	@if [ -f "$(LIBV_ABS)/Makefile" ]; then \
		$(MAKE) -C "$(LIBV_ABS)" CC="$(CC)" CFLAGS="$(CFLAGS)"; \
	fi

src-build:
	$(MAKE) -C src all TOPDIR=$(TOPDIR) BUILD_DIR=$(BUILD_DIR) \
		CC=$(CC) CFLAGS="$(CFLAGS)" CPPFLAGS="$(SRC_CPPFLAGS)" \
		MUSTDB_INCLUDE=$(MUSTDB_INCLUDE) LIBV_INCLUDEDIR=$(LIBV_INCLUDEDIR_ABS)

$(STATIC_LIB): src-build
	$(AR) rcs $@ $(OBJS)

$(SHARED_LIB): libv-local src-build
	$(CC) -shared -o $@ $(OBJS) $(SRC_LDFLAGS) $(LDLIBS)

compat-libs: $(STATIC_LIB) $(SHARED_LIB)
	cp $(STATIC_LIB) src/$(STATIC_LIB)
	cp $(SHARED_LIB) src/$(SHARED_LIB)

clean:
	$(MAKE) -C src clean TOPDIR=$(TOPDIR) BUILD_DIR=$(BUILD_DIR) \
		MUSTDB_INCLUDE=$(MUSTDB_INCLUDE) LIBV_INCLUDEDIR=$(LIBV_INCLUDEDIR_ABS)
	rm -rf $(BUILD_DIR)
	rm -f $(STATIC_LIB) $(SHARED_LIB) src/$(STATIC_LIB) src/$(SHARED_LIB)

distclean: clean
	rm -f Makefile

install: all
	$(INSTALL) -d "$(DESTDIR)$(LIBDIR)"
	$(INSTALL) -d "$(DESTDIR)$(INCLUDEDIR)/mustdb"
	$(INSTALL) -m 644 $(STATIC_LIB) "$(DESTDIR)$(LIBDIR)/"
	$(INSTALL) -m 755 $(SHARED_LIB) "$(DESTDIR)$(LIBDIR)/"
	$(INSTALL) -m 644 $(MUSTDB_INCLUDE)/*.h "$(DESTDIR)$(INCLUDEDIR)/mustdb/"

uninstall:
	-rm -f "$(DESTDIR)$(LIBDIR)/$(STATIC_LIB)"
	-rm -f "$(DESTDIR)$(LIBDIR)/$(SHARED_LIB)"
	-rm -rf "$(DESTDIR)$(INCLUDEDIR)/mustdb"

test: all
	$(MAKE) -C tmp test CC=$(CC) CFLAGS="$(CFLAGS)" CPPFLAGS="$(CPPFLAGS)" \
		LDFLAGS="$(LDFLAGS)" LIBS="$(LIBS)" LIBV_DIR="$(LIBV_ABS)" \
		LIBV_INCLUDEDIR="$(LIBV_INCLUDEDIR_ABS)" LIBV_LIBDIR="$(LIBV_LIBDIR_ABS)" \
		LIBV_LIBS="$(LIBV_LIBS)"
