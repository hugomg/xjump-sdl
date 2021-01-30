PREFIX = /usr/local
BINDIR = $(PREFIX)/bin
DATADIR = $(PREFIX)/share

IMAGEDIR = $(DATADIR)/xjump

CFLAGS  = -std=c11 -pedantic -Wall -Wextra -O2
LDFLAGS =

SDL_CFLAGS = `pkg-config sdl2 --cflags`
SDL_LIBS   = `pkg-config sdl2 --libs`

xjump: main.o
	$(CC) $(LDFLAGS) $< $(SDL_LIBS) -o $@

main.o: main.c
	$(CC) $(SDL_CFLAGS) $(CFLAGS) -DIMAGEDIR='"$(IMAGEDIR)"' -c $< -o $@

#
# Phony targets
#
# TODO: install

.PHONY: clean

clean:
	rm -rf ./*.o xjump
