PROJECT := libv
TOPDIR := $(CURDIR)
BUILD_DIR := $(TOPDIR)/build
LIBV_INCLUDE := $(TOPDIR)/src/include
CC ?= cc
AR ?= ar
CFLAGS ?= -std=c11 -Wall -Wextra -Werror -O2 -g -fPIC
CPPFLAGS += -I$(LIBV_INCLUDE)
PREFIX ?= /usr/local
LIBDIR ?= $(PREFIX)/lib
INCLUDEDIR ?= $(PREFIX)/include

CORE_SRCS := src/hash.c src/vector.c src/vb_type.c
CORE_OBJS := $(patsubst src/%.c,$(BUILD_DIR)/obj/core/%.o,$(CORE_SRCS))
SUBDIRS := src/net
MODULE_OBJS := $(foreach dir,$(SUBDIRS),$(shell $(MAKE) --no-print-directory -s -C $(dir) print-objs TOPDIR=$(TOPDIR) BUILD_DIR=$(BUILD_DIR) LIBV_INCLUDE=$(LIBV_INCLUDE)))
OBJS := $(CORE_OBJS) $(MODULE_OBJS)
DEPFILES := $(CORE_OBJS:.o=.d)

STATIC_LIB := libv.a
SHARED_LIB := libv.so
TEST_SRCS := $(wildcard tests/test_*.c)
TEST_EXES := $(TEST_SRCS:.c=)

.NOTPARALLEL:
.PHONY: all clean install uninstall test subdirs $(SUBDIRS)

all: subdirs $(STATIC_LIB) $(SHARED_LIB)

subdirs: $(SUBDIRS)

$(SUBDIRS):
	$(MAKE) -C $@ all TOPDIR=$(TOPDIR) BUILD_DIR=$(BUILD_DIR) LIBV_INCLUDE=$(LIBV_INCLUDE)

$(MODULE_OBJS): subdirs

$(BUILD_DIR)/obj/core/%.o: src/%.c
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c $< -o $@

$(STATIC_LIB): $(OBJS)
	$(AR) rcs $@ $(OBJS)

$(SHARED_LIB): $(OBJS)
	$(CC) -shared -o $@ $(OBJS)

tests/%: tests/%.c $(STATIC_LIB)
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ $< $(STATIC_LIB) -pthread

test: $(TEST_EXES)
	@for t in $(TEST_EXES); do ./$$t || exit 1; done
	@echo "libv tests passed."

clean:
	@for dir in $(SUBDIRS); do \
		$(MAKE) -C $$dir clean TOPDIR=$(TOPDIR) BUILD_DIR=$(BUILD_DIR) LIBV_INCLUDE=$(LIBV_INCLUDE); \
	done
	rm -rf $(BUILD_DIR)
	rm -f $(STATIC_LIB) $(SHARED_LIB) $(TEST_EXES)

install: all
	install -d "$(DESTDIR)$(LIBDIR)"
	install -d "$(DESTDIR)$(INCLUDEDIR)/libv"
	install -m 644 $(STATIC_LIB) "$(DESTDIR)$(LIBDIR)/"
	install -m 755 $(SHARED_LIB) "$(DESTDIR)$(LIBDIR)/"
	install -m 644 $(LIBV_INCLUDE)/*.h "$(DESTDIR)$(INCLUDEDIR)/libv/"

uninstall:
	-rm -f "$(DESTDIR)$(LIBDIR)/$(STATIC_LIB)"
	-rm -f "$(DESTDIR)$(LIBDIR)/$(SHARED_LIB)"
	-rm -rf "$(DESTDIR)$(INCLUDEDIR)/libv"

-include $(DEPFILES)
