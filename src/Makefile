## Variables overridden by parent Makefile or `configure`
CC ?= cc
CFLAGS ?= -O2 -fPIC -I.
LIBV_DIR ?= ../libv
CFLAGS += -I$(LIBV_DIR)/src/include
PREFIX ?= /usr/local
LIBDIR ?= $(PREFIX)/lib
INCLUDEDIR ?= $(PREFIX)/include

LIBV_OBJS = $(LIBV_DIR)/build/obj/core/hash.o \
            $(LIBV_DIR)/build/obj/core/vector.o \
            $(LIBV_DIR)/build/obj/core/mustdb_type.o \
            $(LIBV_DIR)/build/obj/net/net.o \
            $(LIBV_DIR)/build/obj/net/conn.o
MOVED_TO_LIBV_SRCS = hash.c vector.c mustdb_type.c
SRCS = $(filter-out $(MOVED_TO_LIBV_SRCS),$(wildcard *.c))
OBJS = $(SRCS:.c=.o)

LIBNAME = mustdb
STATIC_LIB = lib$(LIBNAME).a
SHARED_LIB = lib$(LIBNAME).so

.PHONY: all clean install uninstall

all: $(STATIC_LIB) $(SHARED_LIB)

$(LIBV_DIR)/libv.a:
	$(MAKE) -C $(LIBV_DIR) CC=$(CC)

$(STATIC_LIB): $(LIBV_DIR)/libv.a $(OBJS)
	ar rcs $@ $(OBJS) $(LIBV_OBJS)

$(SHARED_LIB): CFLAGS += -fPIC
$(SHARED_LIB): $(LIBV_DIR)/libv.a $(OBJS)
	$(CC) -shared -o $@ $(OBJS) $(LIBV_OBJS)

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(OBJS) $(STATIC_LIB) $(SHARED_LIB)
	$(MAKE) -C $(LIBV_DIR) clean

install: all
	@echo "Installing to $(DESTDIR)$(LIBDIR) and $(DESTDIR)$(INCLUDEDIR)"
	install -d "$(DESTDIR)$(LIBDIR)"
	install -d "$(DESTDIR)$(INCLUDEDIR)"
	install -d "$(DESTDIR)$(INCLUDEDIR)/libv"
	install -m 644 $(STATIC_LIB) "$(DESTDIR)$(LIBDIR)/"
	install -m 755 $(SHARED_LIB) "$(DESTDIR)$(LIBDIR)/"
	install -m 644 $(LIBV_DIR)/src/include/*.h "$(DESTDIR)$(INCLUDEDIR)/libv/"
	# install headers
	for h in *.h; do \
		if [ -f "$$h" ]; then install -m 644 "$$h" "$(DESTDIR)$(INCLUDEDIR)/"; fi; \
	done

uninstall:
	-rm -f "$(DESTDIR)$(LIBDIR)/$(STATIC_LIB)"
	-rm -f "$(DESTDIR)$(LIBDIR)/$(SHARED_LIB)"
	-for h in *.h; do \
		if [ -f "$(DESTDIR)$(INCLUDEDIR)/$$h" ]; then rm -f "$(DESTDIR)$(INCLUDEDIR)/$$h"; fi; \
	done
