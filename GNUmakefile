
# Configuration
# -------------
# The variables that are in all-caps may overriden either via the configure
# script or via the command-line parameters when you invoke make.
# The variables in lowercase should only be changed via the configure script.

CFLAGS     = -std=c11 -pedantic -Wall -Wextra -O2 -g
LIBS       =
SDL_CFLAGS = `pkg-config sdl2 --cflags`
SDL_LIBS   = `pkg-config sdl2 --libs`

INSTALL = install
INSTALL_PROGRAM = $(INSTALL)
INSTALL_DATA    = $(INSTALL) -m 644

include config.mk


# Standard targets
# ----------------
# TODO: install a desktop file + icon

all: xjump

clean:
	rm -rf ./*.o xjump

distclean: clean
	rm -rf config.h config.mk

install:
	$(INSTALL_PROGRAM) -D xjump $(DESTDIR)$(bindir)/xjump
	$(INSTALL_DATA) -D data/ui-font.bmp          $(DESTDIR)$(datadir)/xjump/ui-font.bmp
	$(INSTALL_DATA) -D data/themes/classic.bmp   $(DESTDIR)$(datadir)/xjump/themes/classic.bmp
	$(INSTALL_DATA) -D data/themes/ion.bmp       $(DESTDIR)$(datadir)/xjump/themes/ion.bmp
	$(INSTALL_DATA) -D data/themes/jumpnbump.bmp $(DESTDIR)$(datadir)/xjump/themes/jumpnbump.bmp

uninstall:
	rm -rf $(DESTDIR)$(bindir)/xjump
	rm -rf $(DESTDIR)$(datadir)/xjump

.PHONY: all clean distclean install uninstall


# Compilation
# -----------

xjump: xjump.o
	$(CC) $(LDFLAGS) $< $(SDL_LIBS) $(LIBS) -o $@

xjump.o: xjump.c config.h
	$(CC) $(CPPFLAGS) $(SDL_CFLAGS) $(CFLAGS) -c $< -o $@

config.mk:
	@echo Please run ./configure first
	@exit 1
