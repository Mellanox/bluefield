SBINDIR = /sbin

# Default target.
all:

# By default, use the Makefile's directory as the vpath.
VPATH := $(dir $(lastword $(MAKEFILE_LIST)))

CFLAGS += -O2 -g -std=gnu99 -Werror \
  -Wall -Wshadow -Wuninitialized -Wstrict-overflow -Wundef \
  -Wold-style-definition -Wwrite-strings

LDFLAGS += -Wl,--fatal-warnings

SOURCES := \
  bootctl.c \

# Grab dependencies, if they exist.
-include $(SOURCES:.c=.d)

%.o: %.c
	$(CC) $(CFLAGS) -MD -MP -c -o $@ $<

bootctl: $(SOURCES:.c=.o)
	$(CC) $(LDFLAGS) -o $@ $^

all: bootctl

install::
	mkdir -p $(DESTDIR)$(SBINDIR)
	cp -f bootctl $(DESTDIR)$(SBINDIR)

clean:
	rm -f *.d *.o bootctl

.PHONY: all install clean
