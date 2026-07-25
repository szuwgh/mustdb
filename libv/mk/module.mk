TOPDIR ?= $(abspath ../..)
BUILD_DIR ?= $(TOPDIR)/build
OBJDIR ?= $(BUILD_DIR)/obj/$(MODULE)
LIBV_INCLUDE ?= $(TOPDIR)/src/include

CPPFLAGS += -I$(LIBV_INCLUDE)
CFLAGS ?= -std=c11 -Wall -Wextra -Werror -O2 -g -fPIC

OBJFILES := $(addprefix $(OBJDIR)/,$(OBJS))
DEPFILES := $(OBJFILES:.o=.d)

.PHONY: all clean print-objs

all: $(OBJFILES)

$(OBJDIR)/%.o: %.c
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c $< -o $@

print-objs:
	@printf '%s\n' $(OBJFILES)

clean:
	rm -rf $(OBJDIR)

-include $(DEPFILES)
