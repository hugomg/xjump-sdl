# Xjump 3.0

This is a reimplementation of the classic Xjump game, built with SDL.
We now live in a Wayland world and the old Xlib implementation was growing increasingly
obsolete and harder to build on modern systems.

<div align="center">
    <img alt="A screenshot of Xjump gameplay" src="misc/screenshot-1.png" />
</div>

## New features and improvements

- New default theme
- Increased the FPS from 40 to 60
- Smooth scrolling animations
- Game window is now resizable
- More responsive controls (when pressing left and right simultaneously)

## Installation

<a href='https://flathub.org/apps/io.github.hugomg.xjump-sdl'>
    <img width='196' alt='Get it on Flathub' src='https://flathub.org/api/badge?svg&locale=en'/>
</a>

## Building from source

**Dependencies:**
You will need the header files for SDL2.
On Debian-based Linux distributions these can be found in the `libsdl2-dev` package.
On Fedora the package you need is `SDL2-devel`.

**Compilation:** Use the provided configure script and makefile.
You can choose the installation location and compilation flags via the configure script.
See `./configure --help` for details.

    ./configure
    make && sudo make install

## FAQ

1. Isn't this the same thing as [GNUjump](http://www.gnu.org/software/gnujump/) aka SDLjump?

    This version of xjump keeps the interface closer to the original xjump.
    We launch straight into the game instead of into a menu.
    The smooth scrolling feature is inspired by GNUjump though :)

2. How do I make things look like they did in the classic xjump?

    Use the following command-line flags: `xjump --hard-scroll --theme classic`
