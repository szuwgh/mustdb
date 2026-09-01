MODULE := core
OBJS := db.o
SUBDIRS := catalog parse storage/buffer storage/index storage/table transaction utils wal

TOPDIR ?= $(abspath ..)
BUILD_DIR ?= $(TOPDIR)/build/mustdb

include mk/module.mk
