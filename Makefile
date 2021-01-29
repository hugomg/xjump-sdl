CFLAGS = -std=c11 -pedantic -Wall -Wextra -O2

SDL_CFLAGS = `pkg-config sdl2 --cflags`
SDL_LIBS = `pkg-config sdl2 --libs`

xjump: main.o
	$(CC) $(LDFLAGS) $< $(SDL_LIBS) -o $@

main.o: main.c
	$(CC) $(SDL_CFLAGS) $(CFLAGS) -c $< -o $@

#
# Phony targets
#
# TODO: install

.PHONY: clean

clean:
	rm -rf ./*.o xjump
