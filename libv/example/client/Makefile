CC ?= cc
CFLAGS ?= -Wall -Wextra -O2

all: client

client: client.c
	$(CC) $(CFLAGS) -o $@ $<

clean:
	rm -f client

.PHONY: all clean