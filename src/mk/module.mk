TOPDIR ?= $(abspath ..)
BUILD_DIR ?= $(TOPDIR)/build/mustdb
OBJDIR ?= $(BUILD_DIR)/obj/$(MODULE)
MUSTDB_INCLUDE ?= $(TOPDIR)/src/include
LIBV_INCLUDEDIR ?= $(TOPDIR)/libv/src/include

CC ?= cc
CFLAGS ?= -O2 -fPIC
override CPPFLAGS += -I$(MUSTDB_INCLUDE) -include stdatomic.h \
	-I$(LIBV_INCLUDEDIR) -I$(LIBV_INCLUDEDIR)/libv

OBJFILES := $(addprefix $(OBJDIR)/,$(OBJS))
SUBDIR_OBJS := $(foreach dir,$(SUBDIRS),$(shell $(MAKE) --no-print-directory -s -C $(dir) print-objs TOPDIR=$(TOPDIR) BUILD_DIR=$(BUILD_DIR) MUSTDB_INCLUDE=$(MUSTDB_INCLUDE) LIBV_INCLUDEDIR=$(LIBV_INCLUDEDIR)))
ALL_OBJS := $(OBJFILES) $(SUBDIR_OBJS)
DEPFILES := $(OBJFILES:.o=.d)

.PHONY: all clean print-objs subdirs $(SUBDIRS)

all: $(OBJFILES) subdirs

subdirs: $(SUBDIRS)

$(SUBDIRS):
	$(MAKE) -C $@ all TOPDIR=$(TOPDIR) BUILD_DIR=$(BUILD_DIR) MUSTDB_INCLUDE=$(MUSTDB_INCLUDE) LIBV_INCLUDEDIR=$(LIBV_INCLUDEDIR)

$(OBJDIR)/%.o: %.c
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c $< -o $@

print-objs:
	@printf '%s\n' $(ALL_OBJS)

clean:
	@for dir in $(SUBDIRS); do \
		$(MAKE) -C $$dir clean TOPDIR=$(TOPDIR) BUILD_DIR=$(BUILD_DIR) MUSTDB_INCLUDE=$(MUSTDB_INCLUDE) LIBV_INCLUDEDIR=$(LIBV_INCLUDEDIR); \
	done
	rm -rf $(OBJDIR)

-include $(DEPFILES)
