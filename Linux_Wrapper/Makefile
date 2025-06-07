CC = gcc
CFLAGS = -Wall -pthread
LIBS = -L /usr/local/include -lpigpio -lrt
INSTALL = `which install`

all: packages lis2dwusb

install: lis2dwusb
	$(INSTALL) ./lis2dwusb /usr/local/bin/lis2dwusb

clean:
	rm -f lis2dwusb

packages:

lis2dwusb: lis2dwusb.c
	$(CC) $(CFLAGS) lis2dwusb.c $(LIBS) -o lis2dwusb