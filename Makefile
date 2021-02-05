
# Configuration
# -------------
# If you want to override these variables, the recommended way is via the
# configure script. That way, everything gets recompiled automatically.

CFLAGS = -std=c11 -pedantic -Wall -Wextra -O2 -g

INSTALL = install
INSTALL_PROGRAM = $(INSTALL)
INSTALL_DATA    = $(INSTALL) -m 644

include config.mk # (generated by ./configure)

# Standard targets
# ----------------
# TODO: install a desktop file + icon

all: xjump

clean:
	rm -rf ./*.o xjump config.h

distclean: clean
	rm -rf config.mk

install:
	$(INSTALL_PROGRAM) -D xjump $(DESTDIR)$(bindir)/xjump
	$(INSTALL_DATA) -D data/font-hs.bmp          $(DESTDIR)$(datadir)/xjump/font-hs.bmp
	$(INSTALL_DATA) -D data/font-ui.bmp          $(DESTDIR)$(datadir)/xjump/font-ui.bmp
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

config.h: config.mk
	@printf "%s" "Generating $@..."
	@rm -rf $@
	@echo '/* Do not edit this file by hand */' >> $@
	@echo '/* It was generated by the build system */' >> $@
	@echo '#define XJUMP_VERSION "$(version)"' >> $@
	@echo '#define XJUMP_PREFIX  "$(prefix)"'  >> $@
	@echo '#define XJUMP_BINDIR  "$(bindir)"'  >> $@
	@echo '#define XJUMP_DATADIR "$(datadir)"' >> $@
	@printf " done\n"
