
# Configuration
# -------------
# The variables that are in all-caps may overriden either via the configure
# script or via the command-line parameters when you invoke make.
# The variables in lowercase should only be changed via the configure script.

CFLAGS     = -std=c11 -pedantic -Wall -Wextra -O2 -g
LIBS       =
SDL2_CFLAGS = `pkg-config sdl2 --cflags`
SDL2_LIBS   = `pkg-config sdl2 --libs`

INSTALL = install
INSTALL_PROGRAM = $(INSTALL)
INSTALL_DATA    = $(INSTALL) -m 644

include config.mk


# Standard targets
# ----------------

all: xjump

clean:
	rm -rf ./*.o xjump

distclean: clean
	rm -rf config.h config.mk

install: all
	$(INSTALL_PROGRAM) -D xjump $(DESTDIR)$(bindir)/xjump
	$(INSTALL_DATA) -D images/ui-font.bmp         $(DESTDIR)$(datadir)/xjump/ui-font.bmp
	$(INSTALL_DATA) -D images/theme-jumpnbump.bmp $(DESTDIR)$(datadir)/xjump/theme-jumpnbump.bmp

uninstall:
	rm -rf $(DESTDIR)$(bindir)/xjump
	rm -rf $(DESTDIR)$(datadir)/xjump

.PHONY: all clean distclean install uninstall


# Compilation
# -----------

xjump: main.o
	$(CC) $(LDFLAGS) $< $(SDL2_LIBS) $(LIBS) -o $@

main.o: main.c config.h
	$(CC) $(CPPFLAGS) $(SDL2_CFLAGS) $(CFLAGS) -c $< -o $@

config.mk:
	@echo Please run ./configure first
	@exit 1
