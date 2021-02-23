// Copyright 1997-1999 Tatsuya Kudoh
// Copyright 1997-1999 Masato Taruishi
// Copyright 2015-2021 Hugo Gualandi
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <https://www.gnu.org/licenses/>.

#include <SDL.h>

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <inttypes.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/file.h>
#include <sys/random.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "config.h"
#define XJUMP_FONTDIR   XJUMP_DATADIR "/xjump"
#define XJUMP_THEMEDIR  XJUMP_DATADIR "/xjump/themes"

//
// Helper functions
// ----------------

static int min(int x, int y)
{
    return (x < y ? x : y);
}

static int max(int x, int y)
{
    return (x > y ? x : y);
}

static int mod(int n, int m)
{
    assert(m > 0);
    int r = n % m;
    return (r >= 0 ? r : r + m);
}

static bool isNullOrEmpty(const char *s)
{
    return (s == NULL) || (*s == '\0');
}

static char *concat(const char **strs)
{;
    size_t len = 0;
    for (const char **s = strs; *s != NULL; s++) {
        len += strlen(*s);
    }
    char *res = malloc(len+1);
    res[0] = '\0';
    for (const char **s = strs; *s != NULL; s++) {
        strcat(res, *s); // O(N^2) but should not matter
    }
    return res;
}

//
// Error handling
// --------------

static void panic(const char *what, const char *fullError)
{
    if (fullError == NULL) fullError = "";
    fprintf(stderr, "Internal error! %s. %s\n", what, fullError);
    exit(1);
}

//
// Command-line arguments & config
// -------------------------------

int isSoftScroll = 1;
char *themePath = XJUMP_THEMEDIR "/jumpnbump.bmp";

static void print_usage(const char * progname)
{
    printf("Usage: %s [OPTIONS]...\n"
           "A jumping game for X.\n"
           "\n"
           "  -h --help        show this help message and exit\n"
           "  -v --version     show version information and exit\n"
           "  --soft-scroll    use Xjump 3.0 scrolling behavior (default)\n"
           "  --hard-scroll    use Xjump 1.0 scrolling behavior\n"
           "  --theme NAME     use a pre-installed sprite theme (eg. --theme=classic)\n"
           "  --graphic FILE   use a custom sprite theme (path to a bitmap file)\n"
           "\n"
           "Alternate themes can be found under %s.\n",
           progname, XJUMP_THEMEDIR);
}

static void print_version()
{
    printf("Xjump version %s\n", XJUMP_VERSION);
}

static void parseCommandLine(int argc, char **argv)
{
    static struct option long_options[] = {
        /* These options set a flag. */
        {"soft-scroll", no_argument, &isSoftScroll, 1},
        {"hard-scroll", no_argument, &isSoftScroll, 0},
        /* These options don’t set a flag */
        {"help",    no_argument,        0, 'h'},
        {"version", no_argument,        0, 'v'},
        {"theme",   required_argument,  0, 't'},
        {"graphic", required_argument,  0, 'g'},
        {0, 0, 0, 0}
    };

    while (1) {
        int option_index = 0;
        int c = getopt_long (argc, argv, "hvt:", long_options, &option_index);
        if (c == -1) break;
        switch (c) {
            case 0:
                // Set a flag
                break;

            case 'h':
                print_usage(argv[0]);
                exit(0);

            case 'v':
                print_version();
                exit(0);

            case 't': {
                const char *ss[] = { XJUMP_THEMEDIR, "/", optarg, ".bmp", NULL };
                themePath = concat(ss);
                break;
            }

            case 'g':
                themePath = optarg;
                break;

            case '?':
                // getopt_long already printed an error message
                exit(1);

            default:
                abort(); // Shoulr never happen
        }
    }
}

//
// Random Number Generator
// -----------------------
//
// References:
// https://www.pcg-random.org
// https://www.pcg-random.org/posts/bounded-rands.html

static uint64_t pcgState = 0x853c49e6748fea9bULL; // Mutable state of the RNG
static uint64_t pcgSeq   = 0xda3e39cb94b95bdbULL; // PCG "sequence" parameter

static void pcg32_init(int64_t seed[2])
{
    pcgState = seed[0];
    pcgSeq = (seed[1] << 1) | 1;
}

static uint32_t pcg32_next()
{
    pcgState = pcgState * 6364136223846793005ULL + pcgSeq;
    uint32_t xorshifted = ((pcgState >> 18u) ^ pcgState) >> 27u;
    uint32_t rot = pcgState >> 59u;
    return (xorshifted >> rot) | (xorshifted << ((-rot) & 31));
}

// Returns an uniformly distributed integer in the range [0,a)
static uint32_t pcg32_bounded(uint32_t n)
{
    uint32_t x, r;
    do {
        x = pcg32_next();
        r = x % n;
    } while (x - r > (-n));
    return r;
}

// Returns an uniformly distributed integer in the range [a, b], inclusive
static uint32_t rnd(uint32_t a, uint32_t b)
{
    return a + pcg32_bounded(b-a+1);
}

//
// Highscores
// ----------

int64_t bestScoreEver  = 0;
int64_t bestScoreToday = 0;
time_t bestScoreExpiration = 0;
FILE *highscoreFile = NULL;

static void highscore_init()
{
    char *highscorePath = NULL;

    // Locate the local highscore file, following the XDG spec
    // https://specifications.freedesktop.org/basedir-spec/basedir-spec-latest.html
    const char *fileName = "xjump-highscores";
    const char *HOME          = getenv("HOME");
    const char *XDG_DATA_HOME = getenv("XDG_DATA_HOME");
    if (!isNullOrEmpty(XDG_DATA_HOME)) {
        const char *ss[] = { XDG_DATA_HOME, "/", fileName, NULL };
        highscorePath = concat(ss);
    } else if (!isNullOrEmpty(HOME)) {
        const char *ss[] = { HOME, "/.local/share/", fileName, NULL };
        highscorePath = concat(ss);
    } else {
        fprintf(stderr, "Could not find highscore directory. $HOME is not set.\n");
        goto done;
    }

    // Open the local highscore file or create it if it does not already exist.
    // We need to use low level open() to this precise combination of RW plus
    // file creation. If we get an error, it's better to get it now than after
    // a long game.
    int flags = O_RDWR | O_CREAT;
    int fd = open(highscorePath, flags, 0666);
    if (fd < 0) {
        perror("Could not open highscore file");
        goto done;
    }

    // Create a more convenient FILE* handle for the highscores.
    highscoreFile = fdopen(fd, "r+");
    if (!highscoreFile) {
        perror("Could not create highscore file handle");
        goto done;
    }

done:
    if (!highscoreFile) {
        fprintf(stderr, "Highscores will not be recorded\n");
    }
    free(highscorePath);
}

static void highscore_read()
{
    rewind(highscoreFile);
    if (1 != fscanf(highscoreFile, "best %ld\n", &bestScoreEver)) { return; }
    if (2 != fscanf(highscoreFile, "today %ld %ld\n", &bestScoreToday, &bestScoreExpiration)) { return; }
}

static void highscore_write()
{
    rewind(highscoreFile);
    ftruncate(fileno(highscoreFile), 0);
    fprintf(highscoreFile, "best %ld\n", bestScoreEver);
    fprintf(highscoreFile, "today %ld %ld\n", bestScoreToday, bestScoreExpiration);
    fflush(highscoreFile);
}

static time_t end_of_day(time_t now)
{
    struct tm *date = localtime(&now);
    date->tm_mday += 1;
    date->tm_hour = 0;
    date->tm_min = 0;
    date->tm_sec = 0;
    return mktime(date);
}

static void highscore_update(int64_t newScore)
{
    time_t now = time(NULL);
    if (highscoreFile) {

        if (0 != flock(fileno(highscoreFile), LOCK_EX)) {
            panic("Could not acquire file lock", strerror(errno));
        }

        highscore_read();

        if (newScore > bestScoreEver) {
            bestScoreEver = newScore;
        }

        if (newScore > bestScoreToday || bestScoreExpiration < now) {
            bestScoreToday = newScore;
            bestScoreExpiration = end_of_day(now);
        }

        highscore_write();

        if (0 != flock(fileno(highscoreFile), LOCK_UN)) {
            panic("Could not release file lock", strerror(errno));
        }
    }
}

//
// Joystick state
// --------------

// This component keeps track of the "joystick" state.
// If both LEFT and RIGHT a pressed at the same time, the most recent one wins.

typedef enum {
    LR_NEUTRAL,
    LR_LEFT,
    LR_RIGHT
} LeftRight;

typedef enum {
    INPUT_JUMP,
    INPUT_LEFT,
    INPUT_RIGHT,
    INPUT_OTHER,
} Input;

static struct {
    LeftRight horizDirection;
    bool isPressing[INPUT_OTHER+1];
} K;

static Input translateHotkey(SDL_Keysym key)
{
    switch (key.scancode) {
        case SDL_SCANCODE_UP:
        case SDL_SCANCODE_DOWN:
        case SDL_SCANCODE_W:
        case SDL_SCANCODE_S:
        case SDL_SCANCODE_SPACE:
        case SDL_SCANCODE_KP_8:
        case SDL_SCANCODE_KP_5:
        case SDL_SCANCODE_KP_2:
            return INPUT_JUMP;

        case SDL_SCANCODE_LEFT:
        case SDL_SCANCODE_A:
        case SDL_SCANCODE_KP_4:
            return INPUT_LEFT;

        case SDL_SCANCODE_RIGHT:
        case SDL_SCANCODE_D:
        case SDL_SCANCODE_KP_6:
            return INPUT_RIGHT;

        default:
            return INPUT_OTHER;
    }
}


static void init_input()
{
    K.horizDirection = LR_NEUTRAL;
    for (int i = 0; i < INPUT_OTHER; i++) {
        K.isPressing[i] = false;
    }
}

static void input_keydown(const SDL_Keysym key)
{
    Input input = translateHotkey(key);
    if (input != INPUT_OTHER) {
        K.isPressing[input] = true;
        switch (input) {
            case INPUT_LEFT:
                K.horizDirection = LR_LEFT;
                break;

            case INPUT_RIGHT:
                K.horizDirection = LR_RIGHT;
                break;

            default:
                break;
        }
    }
}

static void input_keyup(const SDL_Keysym key)
{
    Input input = translateHotkey(key);
    if (input != INPUT_OTHER) {
        K.isPressing[input] = false;
        switch (input) {
            case INPUT_LEFT:
                K.horizDirection = (K.isPressing[INPUT_RIGHT] ? LR_RIGHT : LR_NEUTRAL);
                break;

            case INPUT_RIGHT:
                K.horizDirection  = (K.isPressing[INPUT_LEFT] ? LR_LEFT : LR_NEUTRAL);
                break;

            default:
                break;
        }
    }
}
//
// Game Logic
// ----------

// HERE BE DRAGONS (SHOULD THIS BE REFACTORED?)
// The game logic that I am using is taken almost directly from the original
// XJump source code, with soft scrolling bolted on top. This causes the soft
// scrolling parts to be a bit unnatural. The original logic is heavily tied
// to the idea that the hero position is it's position in pixels. However, in
// soft scrolling mode this is no longer true because the screen can scroll
// between frames. The end result is stuff like forcedScroll and interpScroll.
//
// Part of me really wants to rewrite this code so that the hero coordinates
// are relative to the bottom of the tower. That would greatly simplify the
// scrolling logic, because that way the scrolling becomes mostly about the
// camera position, not the hero position. But that will wait for another day
// because I don't want to break working code.
//
// We should also consider if we really want to keep supporting the legacy
// --hard-scroll mode. It took a lot of effort to stamp out all the scrolling
// bugs and in the end the hard scroll logic was completely different than the
// --soft-scroll one...

#define S 16  /* Size of a sprite tile, in pixels */
#define R 32  /* Size of the player sprite, in pixels */

#define FIELD_W 32 /* Width of playing field, in tiles */
#define FIELD_H 24 /* Height of playing field, in tiles */
#define FIELD_EXTRA 3 /* Number of extra rows that we have to draw, to support scrolling */

#define NFLOORS 64    /* Number of floors held in memory */

#define GAME_SPEED 25 /* (40 FPS) Time per simulation frame, in milliseconds */
#define MAX_SCROLL_SPEED 5000  /* scrollCount increment per frame, at max speed */
#define SCROLL_THRESHOLD 20000 /* scrollCount that triggers a frame change */

static const int leftLimit = S;                  // x coordinate that collides with left
static const int rightLimit = (FIELD_W-1)*S - R; // x coordinate that collides with right
static const int topLimit = 5*S;        // y coordinate that triggers a forced scroll
static const int botLimit = FIELD_H*S;  // y coordinate that triggers a game over

typedef struct {
    int left;
    int right;
} Floor;

static struct {

    int64_t score;

    // Physics
    int x, y;   // Top-left of the hero sprite, relative to top-left of screen.
    int vx, vy; // Speed. vy is in pixels per frame but vx is in half-pixels.
    int jump;   // Lowers the gravity during the rising arc of jump, if JUMP button is held.

    // Animations
    int isStanding;
    int isFacingRight;
    int isIdleVariant;
    int idleCount;

    // Scrolling
    int hasStarted;   // Don't start scrolling until we jump for the first time
    int floorOffset;  // Tile height of the row at the top of the screen
    int forcedScroll; // Additional scroll distance in pixels. Happens when you get close to the top.
    int scrollCount;
    int scrollSpeed;

    // Floors
    int fpos;
    int next_floor;
    Floor floors[NFLOORS];
} G;

static Floor *get_floor(int n)
{
    return &G.floors[mod(n, NFLOORS)];
}

static void generate_floor()
{
    // Floor positions are measured in tiles and are stored in a circular
    // buffer. The left and right positions are inclusive, ranging [1,30].
    // The left and right walls are in positions 0 and 31, respectively.
    // The "origin" of each floor ranges [5,26] and is encoded by the fpos
    // variable, which can range between [0,21]. There can be between 2-4
    // tiles to the left and to the right of the origin, totaling 5-9 tiles.
    int n = G.next_floor++;
    Floor *floor = get_floor(n);
    if (n % 250 == 0) {
        floor->left  = 1;
        floor->right = 30;
    } else if (n % 5 == 0) {
        int sign = (rnd(0,1) ? +1 : -1);
        int magnitude = rnd(5,9);
        G.fpos = mod(G.fpos + sign*magnitude, 22);
        floor->left  = G.fpos+5 - rnd(2,4);
        floor->right = G.fpos+5 + rnd(2,4);
    } else {
        floor->left  = -10;
        floor->right = -20;
    }
}

static void init_game()
{
    G.score = 0;

    G.x    = (FIELD_W/2)*S - R/2;
    G.y    = (FIELD_H-4)*S - R;
    G.vx   = 0;
    G.vy   = 0;
    G.jump = 0;

    G.isStanding    = true;
    G.isFacingRight = false;
    G.isIdleVariant = false;
    G.idleCount     = 0;

    G.hasStarted   = false;
    G.floorOffset  = 20;
    G.forcedScroll = 0;
    G.scrollCount  = 0;
    G.scrollSpeed  = 0;

    G.fpos = rnd(0,21);
    G.next_floor = -3;
    for (int i=0; i < NFLOORS; i++) {
        generate_floor();
    }
}

static void scroll()
{
    generate_floor();
    G.floorOffset += 1;
    G.y += S;
    if (G.forcedScroll >= S) {
        G.forcedScroll -= S;
    }
}

static bool isStanding(int hx, int hy)
{
    if (G.vy < 0) {
        return false;
    }

    int y = (hy + R)/S;
    if (y >= FIELD_H) {
        return false;
    }

    // We're standing as long as 8/32 pixels touch the ground.
    const Floor *fl = get_floor(G.floorOffset - y);
    return (fl->left*S - 24 <= hx && hx <= fl->right*S + 8);
}

static int collideWithFloor(int hy) {
    return (hy / S) * S;
}

static bool updateGame()
{
    G.x += G.vx / 2;
    G.y += G.vy;

    // First we collide with the walls, setting the x coordinate.
    // The original version of xjump just set the x coordinate glued to the wall. This version makes
    // the walls subtly bouncier by taking into account the X velocity after the bounce. It's subtle
    // but feels better, IMO, specially if you are just bouncing off the walls before the game
    // starts. The "-2" in the formula is a dampening factor to avoid "flickering" 1px bounces.
    if (G.x < leftLimit && G.vx <= 0) {
        G.x  = leftLimit + max(0, leftLimit - G.x - 2)/2;
        G.vx = -G.vx/2;
    }

    if (G.x > rightLimit && G.vx >= 0) {
        G.x  = rightLimit - max(0, G.x - rightLimit - 2)/2;
        G.vx = -G.vx/2;
    }

    // Next we collide with the floors, setting the y coordinate.
    // This must be after the wall collisions because it depends on the x.
    G.isStanding = isStanding(G.x, G.y);
    if (G.isStanding) {
        G.y = collideWithFloor(G.y);
        G.vy = 0;

        int n = (G.floorOffset - (G.y + R)/S) / 5;
        if (n > G.score) {
            G.score = n;
        }

        if (++G.idleCount >= 5) {
          G.isIdleVariant = !G.isIdleVariant;
          G.idleCount = 0;
        }

        if (K.isPressing[INPUT_JUMP]) {
          G.jump = abs(G.vx)/4+7;
          G.vy = -G.jump/2-12;
          G.isStanding = true;
          if (!G.hasStarted) {
              G.hasStarted = true;
              G.scrollSpeed = 200;
          }
        }
    }

    int accelx = (G.isStanding ? 3 : 2);
    switch (K.horizDirection) {
        case LR_LEFT:
            G.vx = max(G.vx - accelx, -32);
            G.isFacingRight = false;
            break;
        case LR_RIGHT:
            G.vx = min(G.vx + accelx, 32);
            G.isFacingRight = true;
            break;
        case LR_NEUTRAL:
            if (G.isStanding) {
                if      (G.vx < -2) G.vx += 3;
                else if (G.vx >  2) G.vx -= 3;
                else                G.vx = 0;
            }
            break;
    }

    if (!G.isStanding) {
        if (G.jump > 0) {
            G.vy   = -G.jump/2 - 12;
            G.jump = (K.isPressing[INPUT_JUMP] ? G.jump-1 : 0);
        } else {
            G.vy = min(G.vy + 2, 16);
            G.jump = 0;
        }
    }

    // Now we scroll the screen.
    // This must be after we know the x and y.
    if (G.hasStarted) {
        G.scrollSpeed = min(MAX_SCROLL_SPEED, G.scrollSpeed + 1);
        G.scrollCount += G.scrollSpeed;
    }

    while (G.scrollCount > SCROLL_THRESHOLD) {
        G.scrollCount -= SCROLL_THRESHOLD;
        scroll();
    }

    // Force scroll if too close to the top. But only if we are in the air, to
    // avoid big jumps in the scroll due to collideWithFloor. (For softscroll
    // mode, we do this in the rendering loop)
    if (!isSoftScroll && !G.isStanding) {
        while (G.y < topLimit) {
            scroll();
        }
    }

    if (G.y + G.forcedScroll >= botLimit) {
        return true;
    }

    return false;
}

//
// Colors
//

static const SDL_Color backgroundColor  = {   0,   0,   0, 255 };
static const SDL_Color textColor        = { 255, 255, 255, 255 };
static const SDL_Color copyrightColor   = {   0, 255,   0, 255 };
static const SDL_Color boxBorderColor   = {   0,   0, 128, 255 };
static const SDL_Color boxColor         = {   0,   0, 255, 255 };
static const SDL_Color scoreBorderColor = { 255, 255, 255, 255 };

//
// Text rendering
// --------------
//
// To preserve the classic Xjump look, we ship with a copies of the fonts that
// the original used. On Fedora you needed package xorg-x11-fonts-100dpi.
//
//   - Courrier Bold Oblique 18pt, 100dpi variant (courBO18)
//   - FixedMedim 10x20
//
// To accurately emulate the classic Xjump look we need to use bitmapped fonts.
// I experimented with using the SDL_TTF library to render the text but the
// True Type fonts only looked nice if we used antialiasing and that doesn't
// match the look that we want... Not to mention that the fonts are not the
// same as the original and that locating system fonts on Linux is a pain.
// The big downside of bitmapped fonts is that they can't represent special
// characters. We are effectively restricted to ACII.
//
// On the matter of font dimensions, both of our fonts are monospaced, where
// the text is arranged in a rectangular grid. However, one subtlety is that in
// the oblique font each letter might reach into the text box for the glyph to
// their right. For this reason, the glyphs in the font file might be arranged
// farther apart than they are in the text.

typedef struct {
    int w, h;   // Dimensions in the text
    int ow, oh; // Dimentions in the sprite file
} FontSize;

static void text_draw_line(
        SDL_Renderer *renderer,
        SDL_Texture *font,
        const FontSize *fz,
        const char *message,
        const SDL_Rect *where)
{
    int w  = fz->w;
    int h  = fz->h;
    int ow = fz->ow;
    int oh = fz->oh;

    int x = where->x;
    int y = where->y;

    for (int i = 0; message[i] != '\0'; i++) {
        char c = message[i];
        if (c < ' ' || '~' < c) { c = 127; } // Default glyph
        int oi = (c - ' ') % 16;
        int oj = (c - ' ') / 16;
        const SDL_Rect src = { oi*ow, oj*oh, ow, oh };
        const SDL_Rect dst = { x + i*w, y + 0*h, ow, oh };
        SDL_RenderCopy(renderer, font, &src, &dst);
    }
}

static void text_set_color(SDL_Texture *font, SDL_Color color)
{
    // This method of setting colors assumes that the original texture has
    // white text on a transparent background.
    SDL_SetTextureColorMod(font, color.r, color.g, color.b);
}


// Boxes around text
// -------------------------

#define boxBorder 4
#define boxPadding 4

static void text_draw_box(SDL_Renderer *renderer, const SDL_Rect *content)
{
    const SDL_Rect padding = {
        content->x - boxPadding,
        content->y - boxPadding,
        content->w + 2*boxPadding,
        content->h + 2*boxPadding,
    };
    const SDL_Rect border = {
       padding.x - boxBorder,
       padding.y - boxBorder,
       padding.w + 2*boxBorder,
       padding.h + 2*boxBorder,
    };

    SDL_SetRenderDrawColor(renderer, boxBorderColor.r, boxBorderColor.g, boxBorderColor.b, boxBorderColor.a);
    SDL_RenderFillRect(renderer, &border);

    SDL_SetRenderDrawColor(renderer, boxColor.r, boxColor.g, boxColor.b, boxColor.a);
    SDL_RenderFillRect(renderer, &padding);
}

//
// Window placement
//

// Parameters

static const int windowMarginTop   = 24;
static const int windowMarginLeft  = 24;
static const int windowMarginInner = 24;

static const int windowMarginBottom = windowMarginTop;
static const int windowMarginRight  = windowMarginLeft;

static const char *titleMsg      = "FALLING TOWER ver " XJUMP_VERSION;
static const char *scoreLabelMsg = "Floor";
static const char *copyrightMsg  = "(C) 1997 ROYALPANDA";
static const char *gameOverMsg   = "Game Over";
static const char *pauseMsg      = "Pause";
static const char *highscoreMsg1 = "High Score";
static const char *highscoreMsg2 = "Today     "; // Please make these two strings have the same length

static const int NscoreDigits = 10;

// Game spritesheet

static const SDL_Rect skySprite   = { 4*R, 0*S, S, S};
static const SDL_Rect LWallSprite = { 4*R, 1*S, S, S};
static const SDL_Rect RWallSprite = { 4*R, 2*S, S, S};
static const SDL_Rect floorSprite = { 4*R, 3*S, S, S};
static const SDL_Rect heroSprite[8] = {
    { 0*R, 0*R, R, R}, // Stand L (1/2)
    { 1*R, 0*R, R, R}, // Stand R (1/2)
    { 2*R, 0*R, R, R}, // Stand L (2/2)
    { 3*R, 0*R, R, R}, // Stand R (2/2)
    { 0*R, 1*R, R, R}, // Jump L
    { 1*R, 1*R, R, R}, // Jump R
    { 2*R, 1*R, R, R}, // Fall L
    { 3*R, 1*R, R, R}, // Fall R
};

SDL_Surface *loadThemeFile(const char *filename)
{
    SDL_Surface *surface = SDL_LoadBMP(filename);
    if (!surface) {
        fprintf(stderr, "Error loading theme file. %s\n.", SDL_GetError());
        return NULL;
    }
    if (surface->w != 4*R + S || surface->h != 2*R) {
        fprintf(stderr, "Theme spritesheet has the wrong dimensions.\n");
        return NULL;
    }
    return surface;
}

//
// App State
//

typedef enum {
    STATE_RUNNING,
    STATE_PAUSED,
    STATE_GAMEOVER,
    STATE_HIGHSCORES,
} GameState;

GameState currState;  // Current screen
GameState lastDrawn;  // CPU optimization: don't redraw static screens.
uint32_t currTime;    // Current time
uint32_t frameTime;   // (if RUNNING)  Moment when we ran the last simulation frame.
uint32_t pauseTime;   // (if PAUSED)   Remaining time difference when we paused (avoids jerky scrolling when unpausing)
uint32_t deathTime;   // (if GAMEOVER) Moment when when we entered the game over screen

static void state_set(GameState state)
{
    switch (state) {
        case STATE_RUNNING:
            if (currState == STATE_PAUSED) {
                frameTime = currTime - pauseTime;
            } else {
                frameTime = currTime;
            }
            break;

        case STATE_PAUSED:
            assert(currState == STATE_RUNNING);
            pauseTime = (currTime - frameTime);
            break;

        case STATE_GAMEOVER:
            deathTime = currTime;
            highscore_update(G.score);
            break;

        case STATE_HIGHSCORES:
            break;
    }
    currState = state;
}

int main(int argc, char **argv)
{
    // Configuration
    parseCommandLine(argc, argv);

    // Initialize subsystems
    if (0 != SDL_Init(SDL_INIT_VIDEO))
        panic("Could not initialize SDL", SDL_GetError());
    atexit(SDL_Quit);

    int64_t seed[2];
    ssize_t nread = getrandom(seed, sizeof(seed), GRND_NONBLOCK);
    if (nread == -1) panic("Could not initialize RNG", strerror(errno));

    pcg32_init(seed);
    highscore_init();
    init_input();
    init_game();

    // Widths and Heights

    static const FontSize uiFZ = { 15, 28, 20, 28 };
    static const FontSize hsFZ = { 10, 20, 10, 20 };

    const int titleW      = uiFZ.w * strlen(titleMsg);
    const int scoreLabelW = uiFZ.w * strlen(scoreLabelMsg);
    const int copyrightW  = uiFZ.w * strlen(copyrightMsg);
    const int gameOverW   = uiFZ.w * strlen(gameOverMsg);
    const int pauseW      = uiFZ.w * strlen(pauseMsg);

    const int textBoxH = uiFZ.h + boxBorder + 2*boxPadding + boxBorder;

    const int gameW = S * FIELD_W;
    const int gameH = S * FIELD_H;

    const int backgroundW = S * FIELD_W;
    const int backgroundH = S * (FIELD_H + FIELD_EXTRA);

    const int scoreDigitsW = uiFZ.w * NscoreDigits;
    const int scoreW = scoreLabelW + uiFZ.w + scoreDigitsW;

    const int windowW = windowMarginLeft + gameW + windowMarginRight;
    const int windowH = windowMarginTop + 3*windowMarginInner + textBoxH + 2*uiFZ.h + + gameH + windowMarginBottom;

    // Screen positions

    const int titleX     = (windowW - titleW)/2;
    const int scoreX     = (windowW - scoreW)/2;
    const int gameX      = (windowW - gameW)/2;
    const int copyrightX = (windowW - copyrightW)/2;

    const int titleY     = windowMarginTop + boxBorder + boxPadding;
    const int scoreY     = titleY + uiFZ.h + boxPadding + boxBorder + windowMarginInner;
    const int gameY      = scoreY + uiFZ.h + windowMarginInner;
    const int copyrightY = gameY + gameH + windowMarginInner;

    const int scoreLabelX  = scoreX;
    const int scoreDigitsX = scoreX + scoreLabelW + uiFZ.w;

    const int gameOverX = gameX + (gameW - gameOverW)/2;
    const int gameOverY = gameY + (gameH - uiFZ.h)*2/5;

    const int pauseX = gameX + (gameW - pauseW)/2;
    const int pauseY = gameY + (gameH - uiFZ.h)*2/5;

    const SDL_Rect titleDst       = { titleX, titleY, titleW, uiFZ.h };
    const SDL_Rect scoreLabelDst  = { scoreLabelX, scoreY, scoreLabelW, uiFZ.h };
    const SDL_Rect scoreDigitsDst = { scoreDigitsX, scoreY, scoreDigitsW, uiFZ.h };
    const SDL_Rect copyrightDst   = { copyrightX, copyrightY, copyrightW, uiFZ.h };
    const SDL_Rect gameOverDst    = { gameOverX, gameOverY, gameOverW, uiFZ.h };
    const SDL_Rect pauseDst       = { pauseX, pauseY, pauseW, uiFZ.h };
    const SDL_Rect gameDst        = { gameX, gameY, gameW, gameH };

    // Load SDL resources

    SDL_Surface *spritesSurface = loadThemeFile(themePath);
    if (!spritesSurface) { exit(1); }

    SDL_Surface *uiFontSurface = SDL_LoadBMP(XJUMP_FONTDIR "/font-ui.bmp");
    if (!uiFontSurface) panic("Could not load font file", SDL_GetError());

    SDL_Surface *hsFontSurface = SDL_LoadBMP(XJUMP_FONTDIR "/font-hs.bmp");
    if (!hsFontSurface) panic("Could not load font file", SDL_GetError());

    SDL_Window *window = SDL_CreateWindow(
        /*title*/ "xjump",
        /*x*/ SDL_WINDOWPOS_UNDEFINED,
        /*y*/ SDL_WINDOWPOS_UNDEFINED,
        /*w*/ windowW,
        /*h*/ windowH,
        /*flags*/ SDL_WINDOW_RESIZABLE);
    if (!window) panic("Could not create window", SDL_GetError());

    SDL_RendererFlags renderFlags = SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC |SDL_RENDERER_TARGETTEXTURE;
    SDL_Renderer *renderer = SDL_CreateRenderer(window, -1, renderFlags);
    if (!renderer) panic("Coult not create SDL renderer", SDL_GetError());

    SDL_Texture *sprites = SDL_CreateTextureFromSurface(renderer, spritesSurface);
    if (!sprites) panic("Could not create texture", SDL_GetError());

    SDL_Texture *uiFont = SDL_CreateTextureFromSurface(renderer, uiFontSurface);
    if (!uiFont) panic("Could not create texture", SDL_GetError());

    SDL_Texture *hsFont = SDL_CreateTextureFromSurface(renderer, hsFontSurface);
    if (!hsFont) panic("Could not create texture", SDL_GetError());

    // At this point, everything we need is loaded to textures
    SDL_FreeSurface(spritesSurface);
    SDL_FreeSurface(uiFontSurface);
    SDL_FreeSurface(hsFontSurface);

    // Create background textures with all the things that don't change from
    // frame to frame. This reduces the number of draw calls in the inner loop.
    SDL_Texture *windowBackground = SDL_CreateTexture(
            renderer, SDL_PIXELFORMAT_RGBA8888, SDL_TEXTUREACCESS_TARGET,
            windowW, windowH);
    if (!windowBackground) panic("Could not create window background texture", SDL_GetError());

    SDL_Texture *gameBackground = SDL_CreateTexture(
            renderer, SDL_PIXELFORMAT_RGBA8888, SDL_TEXTUREACCESS_TARGET,
            backgroundW, backgroundH + S);
    if (!gameBackground) panic("Could not create game background texture", SDL_GetError());

    text_set_color(uiFont, textColor);

    {
        SDL_SetRenderTarget(renderer, windowBackground);

        SDL_SetRenderDrawColor(renderer, backgroundColor.r,  backgroundColor.g, backgroundColor.b, backgroundColor.a);
        SDL_RenderClear(renderer);

        text_draw_box(renderer, &titleDst);
        text_draw_line(renderer, uiFont, &uiFZ, titleMsg, &titleDst);
        text_draw_line(renderer, uiFont, &uiFZ, scoreLabelMsg, &scoreLabelDst);

        text_set_color(uiFont, copyrightColor);
        text_draw_line(renderer, uiFont, &uiFZ, copyrightMsg, &copyrightDst);
        text_set_color(uiFont, textColor);

        SDL_RenderPresent(renderer);
        SDL_SetRenderTarget(renderer, NULL);
    }

    {
        SDL_SetRenderTarget(renderer, gameBackground);

        SDL_SetTextureBlendMode(gameBackground, SDL_BLENDMODE_BLEND);
        SDL_SetRenderDrawColor(renderer, 0,  0, 0, 0);
        SDL_RenderClear(renderer);

        // Background
        for (int y = 0; y < FIELD_H + FIELD_EXTRA; y++) {
            for (int x = 0; x < FIELD_W; x++) {
                const SDL_Rect *src = ((x == 0) ? &LWallSprite : (x == FIELD_W-1) ? &RWallSprite : &skySprite);
                const SDL_Rect dst = { x*S, y*S, S, S };
                SDL_RenderCopy(renderer, sprites, src, &dst);
            }
        }

        // Wide floor
        for (int x = 0; x < FIELD_W; x++) {
            const SDL_Rect dst = { x*S, backgroundH, S, S };
            SDL_RenderCopy(renderer, sprites, &floorSprite, &dst);
        }

        SDL_RenderPresent(renderer);
        SDL_SetRenderTarget(renderer, NULL);
    }

    // Tell the renderer to stretch the drawing if the window is resized
    SDL_RenderSetLogicalSize(renderer, windowW, windowH);

    state_set(STATE_RUNNING);

    while (1) {

        currTime = SDL_GetTicks();

        //
        // Respond to events
        //

        bool wasResized = false;

        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            switch (e.type) {
                case SDL_QUIT:
                    goto quit;

                case SDL_KEYUP: {
                    SDL_Keysym key = e.key.keysym;
                    input_keyup(key);
                    break;
                }

                case SDL_KEYDOWN: {
                    SDL_Keysym key = e.key.keysym;
                    input_keydown(key);
                    if (key.sym == SDLK_q && (key.mod & KMOD_SHIFT)) {
                        goto quit;
                    }
                    switch (currState) {
                        case STATE_RUNNING:
                            if (key.sym == SDLK_p
                             || key.sym == SDLK_PAUSE) {
                                state_set(STATE_PAUSED);
                            }
                            break;

                        case STATE_PAUSED:
                            state_set(STATE_RUNNING);
                            break;

                        case STATE_GAMEOVER:
                            state_set(STATE_HIGHSCORES);
                            break;

                        case STATE_HIGHSCORES:
                            init_input();
                            init_game();
                            state_set(STATE_RUNNING);
                            break;
                    }
                    break;
                }

                case SDL_WINDOWEVENT:
                    switch (e.window.event) {
                        case SDL_WINDOWEVENT_FOCUS_LOST:
                            if (currState == STATE_RUNNING) {
                                state_set(STATE_PAUSED);
                            }
                            break;


                        case SDL_WINDOWEVENT_RESIZED:
                        case SDL_WINDOWEVENT_SIZE_CHANGED:
                        case SDL_WINDOWEVENT_MINIMIZED:
                        case SDL_WINDOWEVENT_MAXIMIZED:
                        case SDL_WINDOWEVENT_RESTORED:
                            wasResized = true;
                            break;
                    }
                    break;

                default:
                    break;
            }
        }

        //
        // Run the current state
        //

        switch (currState) {
            case STATE_RUNNING:
                while (frameTime + GAME_SPEED <= currTime) {
                    frameTime += GAME_SPEED;
                    if (updateGame()) {
                        state_set(STATE_GAMEOVER);
                        break;
                    }
                }
                break;

            case STATE_GAMEOVER:
                if (deathTime + 2000 <= currTime) {
                    state_set(STATE_HIGHSCORES);
                }
                break;

            case STATE_PAUSED:
            case STATE_HIGHSCORES:
                // Nothing
                break;
        }

        //
        // Draw
        //

        bool needsRepaint = (currState == STATE_RUNNING || currState != lastDrawn || wasResized);
        if (needsRepaint) {

            SDL_SetRenderDrawColor(renderer, backgroundColor.r,  backgroundColor.g, backgroundColor.b, backgroundColor.a);
            SDL_RenderClear(renderer);
            SDL_RenderCopy(renderer, windowBackground, NULL, NULL);

            char scoreDigits[32];
            snprintf(scoreDigits, sizeof(scoreDigits), "%010ld", G.score);
            text_draw_line(renderer, uiFont, &uiFZ, scoreDigits, &scoreDigitsDst);

            if (currState == STATE_HIGHSCORES)  {

                // Clear background
                SDL_SetRenderDrawColor(renderer, scoreBorderColor.r,  scoreBorderColor.g, scoreBorderColor.b, scoreBorderColor.a);
                SDL_RenderFillRect(renderer, &gameDst);

                const SDL_Rect innerRect = { gameX+1, gameY+1, gameW-2, gameH-2 };
                SDL_SetRenderDrawColor(renderer, backgroundColor.r,  backgroundColor.g, backgroundColor.b, backgroundColor.a);
                SDL_RenderFillRect(renderer, &innerRect);

                // Draw the high scores
                // To avoid showing repeated high scores in the first day the
                // person is playing, only show the best time today if it is
                // different. This also gives a nice visual cue if you get an
                // all time highscore :)
                char lines[2][32];

                snprintf(lines[0], sizeof(lines[0]), "%s %6ld", highscoreMsg1, bestScoreEver);
                snprintf(lines[1], sizeof(lines[1]), "%s %6ld", highscoreMsg2, bestScoreToday);

                int N = (bestScoreToday != bestScoreEver ? 2 : 1);

                int highscoreW = hsFZ.w * 17;
                int highscoreH = hsFZ.h * N;
                int highscoreX = gameX + (gameW - highscoreW)/2;
                int highscoreY = gameY + (gameH - highscoreH)/2;

                for (int i = 0; i < N; i ++) {
                    const SDL_Rect dst = { highscoreX, highscoreY + i*hsFZ.h, highscoreW, hsFZ.h };
                    text_draw_line(renderer, hsFont, &hsFZ, lines[i], &dst);
                }

            } else {
                SDL_RenderSetClipRect(renderer, &gameDst);

                int sx, sy, interpScroll;
                if (!isSoftScroll) {
                    // In hard scroll more we don't interpolate the hero
                    // position at all because it causes too much flickering
                    // during forced scrolls
                    sx = G.x;
                    sy = G.y;
                    interpScroll = 0;
                } else {
                    // In soft scroll mode, we compute the hero and scroll
                    // coordinates using linear interpolation.

                    // Predict current hero position (without scroll)
                    int dt = currTime - frameTime;
                    int hx = G.x + (G.vx/2)*dt/GAME_SPEED;
                    if (hx < leftLimit) { hx = leftLimit; }
                    if (hx > rightLimit) { hx = rightLimit; }
                    int hy = G.y + (G.vy)*dt/GAME_SPEED;
                    int stand = isStanding(hx, hy);
                    if (stand) { hy = collideWithFloor(hy); }

                    // Predict current hero position (with scroll)
                    int c = G.scrollCount + dt*G.scrollSpeed/GAME_SPEED;
                    sx = hx;
                    sy = hy + G.forcedScroll + S*c/SCROLL_THRESHOLD;
                    if (!stand && sy < topLimit) {
                        G.forcedScroll += (topLimit - sy);
                        G.scrollCount = 0;
                        sy = topLimit;
                    }

                    interpScroll = sy - hy;
                }

                // Background
                const SDL_Rect backgroundSrc = { 0, 0, backgroundW, backgroundH };
                const SDL_Rect backgroundDst = { gameX, gameY - S*FIELD_EXTRA + interpScroll, backgroundW, backgroundH };
                SDL_RenderCopy(renderer, gameBackground, &backgroundSrc, &backgroundDst);

                // Floors
                for (int y = -FIELD_EXTRA; y < FIELD_H; y++) {
                    const Floor *floor = get_floor(G.floorOffset - y);
                    int xl = floor->left;
                    int xr = floor->right;
                    if (xl <= xr) {
                        int w = xr - xl + 1;
                        const SDL_Rect src = { 0, backgroundH, w*S, S };
                        const SDL_Rect dst = { gameX + xl*S, gameY + y*S + interpScroll, w*S, S };
                        SDL_RenderCopy(renderer, gameBackground, &src, &dst);
                    }
                }

                // Hero sprite
                int isFlying  = !G.isStanding;
                int isRight   = G.isFacingRight;
                int isVariant = (G.isStanding? G.isIdleVariant : (G.vy > 0));
                int sprite_index = (isFlying&1) << 2 | (isVariant&1) << 1 | (isRight&1) << 0;
                const SDL_Rect heroDst = { gameX + sx, gameY + sy, R, R };
                SDL_RenderCopy(renderer, sprites, &heroSprite[sprite_index], &heroDst);

                // Text box
                if (currState == STATE_GAMEOVER) {
                    text_draw_box(renderer, &gameOverDst);
                    text_draw_line(renderer, uiFont, &uiFZ, gameOverMsg, &gameOverDst);
                }
                if (currState == STATE_PAUSED) {
                    text_draw_box(renderer, &pauseDst);
                    text_draw_line(renderer, uiFont, &uiFZ, pauseMsg, &pauseDst);
                }

                if (isSoftScroll) {
                    // We need to do this after drawing the floors, otherwise it
                    // messes up the G.floorOffset
                    while (G.forcedScroll >= S) {
                        scroll();
                    }
                }

                SDL_RenderSetClipRect(renderer, NULL);
            }

            SDL_RenderPresent(renderer);
            lastDrawn = currState;

        } else {

            // Normally, the game yields the CPU when it calls RenderPresent, due to the
            // PRESENTVSYNC setting. However, when we don't draw anything to the screen
            // we have to put the program to sleep ourselves to prevent the game from
            // using 100% of the CPU.
            SDL_Delay(GAME_SPEED);
        }
    }

quit:
    return 0;
}
