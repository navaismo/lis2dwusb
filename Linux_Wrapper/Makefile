CC ?= cc
CFLAGS ?= -O2 -std=c11 -Wall -Wextra -Wpedantic -Werror
PREFIX ?= /usr/local
DESTDIR ?=

.PHONY: all install clean
all: lis2dwusb

lis2dwusb: lis2dwusb.c ../include/lis2dw_protocol.h
	$(CC) $(CPPFLAGS) $(CFLAGS) $< $(LDFLAGS) -o $@

install: lis2dwusb
	install -d $(DESTDIR)$(PREFIX)/bin
	install -m 755 lis2dwusb $(DESTDIR)$(PREFIX)/bin/lis2dwusb

clean:
	$(RM) lis2dwusb
