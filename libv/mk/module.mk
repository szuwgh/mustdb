TOPDIR ?= $(abspath ../..)
BUILD_DIR ?= $(TOPDIR)/build
OBJDIR ?= $(BUILD_DIR)/obj/$(MODULE)
LIBV_INCLUDE ?= $(TOPDIR)/src/include

override CPPFLAGS += -I$(LIBV_INCLUDE)
CFLAGS ?= -std=c11 -Wall -Wextra -Werror -O2 -g -fPIC

OBJFILES := $(addprefix $(OBJDIR)/,$(OBJS))
SUBDIR_OBJS := $(foreach dir,$(SUBDIRS),$(shell $(MAKE) --no-print-directory -s -C $(dir) print-objs TOPDIR=$(TOPDIR) BUILD_DIR=$(BUILD_DIR) LIBV_INCLUDE=$(LIBV_INCLUDE)))
ALL_OBJS := $(OBJFILES) $(SUBDIR_OBJS)
DEPFILES := $(OBJFILES:.o=.d)

.PHONY: all clean print-objs subdirs $(SUBDIRS)

all: $(OBJFILES) subdirs

subdirs: $(SUBDIRS)

$(SUBDIRS):
	$(MAKE) -C $@ all TOPDIR=$(TOPDIR) BUILD_DIR=$(BUILD_DIR) LIBV_INCLUDE=$(LIBV_INCLUDE)

$(OBJDIR)/%.o: %.c
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c $< -o $@

print-objs:
	@printf '%s\n' $(ALL_OBJS)

clean:
	@for dir in $(SUBDIRS); do \
		$(MAKE) -C $$dir clean TOPDIR=$(TOPDIR) BUILD_DIR=$(BUILD_DIR) LIBV_INCLUDE=$(LIBV_INCLUDE); \
	done
	rm -rf $(OBJDIR)

-include $(DEPFILES)
