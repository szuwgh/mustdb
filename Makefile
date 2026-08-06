CC = cc
CFLAGS = -O2 -fPIC
PREFIX = /home/postgres/cproject/mustdb/libmustdb
LIBDIR = /home/postgres/cproject/mustdb/libmustdb/lib
INCLUDEDIR = /home/postgres/cproject/mustdb/libmustdb/include

.PHONY: all clean install uninstall test

all:
	$(MAKE) -C src all CC=$(CC) CFLAGS="$(CFLAGS)"

clean:
	$(MAKE) -C src clean

install:
	$(MAKE) -C src install CC=$(CC) CFLAGS="$(CFLAGS)" \
		PREFIX=$(PREFIX) LIBDIR=$(LIBDIR) INCLUDEDIR=$(INCLUDEDIR)

uninstall:
	$(MAKE) -C src uninstall PREFIX=$(PREFIX) LIBDIR=$(LIBDIR) INCLUDEDIR=$(INCLUDEDIR)

test:
	$(MAKE) -C tmp test CC=$(CC)
