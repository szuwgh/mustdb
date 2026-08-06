EXTENSION = pg_mustdb
MODULE_big = pg_mustdb

SRCDIR = src

# 找所有 .c 文件（递归）
SRCS = $(shell find $(SRCDIR) -name "*.c")
PG_CPPFLAGS += -Isrc/include
# 转换为 .o
OBJS = $(SRCS:.c=.o)

DATA = pg_mustdb--0.1.0.sql

UD_CONFIG ?= /home/unvdb/unvdb-tx/bin/ud_config
PG_CONFIG = $(UD_CONFIG)

PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)




