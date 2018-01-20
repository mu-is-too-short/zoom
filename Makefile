# @(#)$Mu: zoom/Makefile 1.7 2001/03/11 06:06:13 $
CC      = gcc
CFLAGS  = -I. -O2 -ansi -pedantic -Wall -Werror -Wmissing-prototypes \
	-DVER=\"$(VERSION)\" -D_POSIX_SOURCE -D_XOPEN_SOURCE
LDFLAGS = -s -L/usr/X11/lib 
LIBS    = -lXext -lX11
VERSION	= 1.4

zoom: zoom.o
	$(CC) $(LDFLAGS) -o zoom zoom.o $(LIBS)

clean:
	rm -f zoom zoom.o
