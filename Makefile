CFLAGS:=-std=c11 -pedantic -Wall -Wextra -O2

SDL_CFLAGS:=`pkg-config SDL2_ttf --cflags`
SDL_LIBS:=`pkg-config SDL2_ttf --libs`

xjump: main.o
	$(CC) $(SDL_LIBS) $< -o $@

%.o: %.c
	$(CC) $(CFLAGS) $(SDL_CFLAGS) -c $< -o $@

main.o: main.c


#
# Phony targets
#

clean:
	rm -rf ./*.o xjump


.PHONY: clean
