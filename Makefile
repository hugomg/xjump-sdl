CFLAGS = -std=c11 -pedantic -Wall -Wextra -O2

SDL_CFLAGS = `pkg-config SDL2_ttf --cflags`
SDL_LIBS = `pkg-config SDL2_ttf --libs`

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
