CC ?= cc
CFLAGS ?= -std=c11 -Wall -Wextra -Werror -O2 -g
LIBV_DIR ?= ../..
CPPFLAGS += -I$(LIBV_DIR)/src/include
LDLIBS += $(LIBV_DIR)/libv.a -pthread

all: server

$(LIBV_DIR)/libv.a:
	$(MAKE) -C $(LIBV_DIR) libv.a

server: server.c $(LIBV_DIR)/libv.a
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ server.c $(LDLIBS)

clean:
	rm -f server

.PHONY: all clean
