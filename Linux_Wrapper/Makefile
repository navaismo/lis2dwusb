CC ?= cc
CFLAGS ?= -O2 -std=c11 -Wall -Wextra -Wpedantic -Werror
PREFIX ?= /usr/local
DESTDIR ?=

.PHONY: all install clean FORCE
all: lis2dwusb

# Copied binaries may target another CPU even when newer than the sources.
lis2dwusb: lis2dwusb.c ../include/lis2dw_protocol.h FORCE
	$(CC) $(CPPFLAGS) $(CFLAGS) $< $(LDFLAGS) -o $@

FORCE:

install: lis2dwusb
	install -d $(DESTDIR)$(PREFIX)/bin
	install -m 755 lis2dwusb $(DESTDIR)$(PREFIX)/bin/lis2dwusb

clean:
	$(RM) lis2dwusb
