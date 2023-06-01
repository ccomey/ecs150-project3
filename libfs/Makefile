# Target library
lib := libfs.a
objs := disk.o fs.o

CC := gcc
CFLAGS := -Wall -Wextra -Werror -MMD
CFLAGS += -g
PANDOC := pandoc

ifneq ($(V),1)
Q=@
endif

all: $(lib)

# Dep Tracking
deps := $(patsubst %.o,%.d,$(objs))
-include $(deps)

libfs.a: $(objs)
	@echo "CC $@"
	$(Q)ar rcs -o $@ $^

%.o: %.c
	@echo "CC $@"
	$(Q)$(CC) $(CFLAGS) -c -o $@ $<

clean:
	@echo "clean"
	$(Q)rm -f $(lib) $(objs) $(deps)

## TODO: Phase 1

