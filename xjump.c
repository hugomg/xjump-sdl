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
#include <sys/random.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>

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

// TODO: global highscores
// https://fedoraproject.org/wiki/SIGs/Games/Packaging
//
// TODO: use flock
// (See the original xjump code)

int64_t best_score = 0;

static void highscore_update(int64_t score)
{
    best_score = score;
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
    // TODO should we use scan codes?
    switch (key.sym) {
        case SDLK_UP:
        case SDLK_DOWN:
        case SDLK_w:
        case SDLK_s:
        case SDLK_SPACE:
            return INPUT_JUMP;

        case SDLK_LEFT:
        case SDLK_a:
            return INPUT_LEFT;

        case SDLK_RIGHT:
        case SDLK_d:
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

static void input_keyup(const SDL_Keysym key)
{
    Input input = translateHotkey(key);
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
//
// Game Logic
// ----------

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

typedef enum {
    STATE_RUNNING,
    STATE_PAUSED,
    STATE_GAMEOVER,
    STATE_HIGHSCORES,
} GameState;

typedef struct {
    int left;
    int right;
} Floor;

static struct {

    int64_t score;

    // Gameover / Pause / Highscores
    GameState state;
    GameState lastDrawn; // CPU optimization: don't redraw static screens.
    int gameOverCount; // How many frames since we hit gameover

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

    G.state = STATE_RUNNING;
    G.lastDrawn = STATE_RUNNING; // (a white lie, but works)
    G.gameOverCount = 0;

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
    G.scrollSpeed  = 200;

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

static void updateRunningGame()
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
          G.hasStarted = true;
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

    if (isSoftScroll) {
        if (G.y + G.forcedScroll < topLimit) {
            G.forcedScroll = topLimit - G.y;
            while (G.forcedScroll >= S) {
                G.scrollCount = 0;
                scroll();
            }
        }
    } else {
        while (G.y < topLimit) {
            scroll();
        }
    }

    if (G.y + G.forcedScroll >= botLimit) {
        G.state = STATE_GAMEOVER;
        highscore_update(G.score);
    }
}

static void updateGame()
{
    switch (G.state) {
        case STATE_GAMEOVER:
            if (++G.gameOverCount >= 80) {
                G.state = STATE_HIGHSCORES;
            }
            break;

        case STATE_PAUSED:
        case STATE_HIGHSCORES:
            // Do nothing
            break;

        case STATE_RUNNING:
            updateRunningGame();
            break;
    }
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
// To preserve the classic Xjump look, we ship with a copy of the font that the
// original used: courBO18 (Courier, 18pt, Bold, Oblique), the 100dpi variant.
// On Fedora it can be installed with sdnf install xorg-x11-fonts-100dpi.
//
// We do have to use a bitmapped font here. Originally I tried using the
// SDL_TTF library to render the text because I was interested in being able to
// render UTF-8 user names. However, the TrueType fonts only looked nice at a
// small font size if we used anti-aliasing, which ruined the Xjump "look". Not
// to mention the headache of finding the font file. We'd probably need to ship
// our own font to avoid adding depending on the system fonts...
//
// In our font bitmap file the glyphs are horizontally spaced slightly farther
// apart than they are in normal text. This is because this is an italic font
// and each glyph can spill a bit into the glyph to its right.

static const int FW = 15; // Width of each glyph in the text
static const int FH = 25; // Height of each glyph in the text

static const int FBW = 20; // Width of each glyph in the font bitmap file
static const int FBH = 25; // Height of each glyph in the font bitmap file

static const int boxBorder = 4;
static const int boxPaddingTop    = 7;
static const int boxPaddingBottom = 5;
static const int boxPaddingLeft   = 4;
static const int boxPaddingRight  = 4;

static void draw_text(
        SDL_Renderer *renderer,
        SDL_Texture *font,
        const char *message,
        SDL_Color color,
        const SDL_Rect *where)
{
    // This method of setting the final color assumes that the font color in
    // the original texture is white.
    SDL_SetTextureColorMod(font, color.r, color.g, color.b);

    for (int i = 0; message[i] != '\0'; i++) {
        char c = message[i];
        if (c < ' ' || '~' < c) { c = 127; } // Default glyph
        int ii = (c - ' ') / 16;
        int jj = (c - ' ') % 16;
        const SDL_Rect src = { jj*FBW, ii*FBH, FBW, FBH };
        const SDL_Rect dst = { where->x + FW*i, where->y, FBW, FBH };
        SDL_RenderCopy(renderer, font, &src, &dst);
    }
}

static void draw_text_box(SDL_Renderer *renderer, const SDL_Rect *content)
{
    const SDL_Rect padding = {
        content->x - boxPaddingLeft,
        content->y - boxPaddingTop,
        content->w + boxPaddingLeft + boxPaddingRight,
        content->h + boxPaddingTop + boxPaddingBottom,
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
static const int windowMarginInner = 32;

static const int windowMarginBottom = windowMarginTop;
static const int windowMarginRight  = windowMarginLeft;

static const char *titleMsg      = "FALLING TOWER ver " XJUMP_VERSION;
static const char *scoreLabelMsg = "Floor";
static const char *copyrightMsg  = "(C) 1997 ROYALPANDA";
static const char *gameOverMsg   = "Game Over";
static const char *pauseMsg      = "Pause";
static const char *highscoreMsg  = "High Score";

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
    init_input();
    init_game();

    // Widths and Heights

    const int titleW      = FW * strlen(titleMsg);
    const int scoreLabelW = FW * strlen(scoreLabelMsg);
    const int copyrightW  = FW * strlen(copyrightMsg);
    const int gameOverW   = FW * strlen(gameOverMsg);
    const int pauseW      = FW * strlen(pauseMsg);

    const int textBoxH = FH + boxBorder + boxPaddingLeft + boxPaddingRight  + boxBorder;

    const int gameW = S * FIELD_W;
    const int gameH = S * FIELD_H;

    const int backgroundW = S * FIELD_W;
    const int backgroundH = S * (FIELD_H + FIELD_EXTRA);

    const int scoreDigitsW = NscoreDigits * FW;
    const int scoreW = scoreLabelW + FW + scoreDigitsW;

    const int windowW = windowMarginLeft + gameW + windowMarginRight;
    const int windowH = windowMarginTop + 3*windowMarginInner + textBoxH + 2*FH + + gameH + windowMarginBottom;

    // Screen positions

    const int titleX     = (windowW - titleW)/2;
    const int scoreX     = (windowW - scoreW)/2;
    const int gameX      = (windowW - gameW)/2;
    const int copyrightX = (windowW - copyrightW)/2;

    const int titleY     = windowMarginTop + boxBorder + boxPaddingTop;
    const int scoreY     = titleY + FH + boxPaddingBottom + boxBorder + windowMarginInner;
    const int gameY      = scoreY + FH + windowMarginInner;
    const int copyrightY = gameY + gameH + windowMarginInner;

    const int scoreLabelX  = scoreX;
    const int scoreDigitsX = scoreX + scoreLabelW + FW;

    const int gameOverX = gameX + (gameW - gameOverW)/2;
    const int gameOverY = gameY + (gameH - FH)*2/5;

    const int pauseX = gameX + (gameW - pauseW)/2;
    const int pauseY = gameY + (gameH - FH)*2/5;

    const SDL_Rect titleDst       = { titleX, titleY, titleW, FH };
    const SDL_Rect scoreLabelDst  = { scoreLabelX, scoreY, scoreLabelW, FH };
    const SDL_Rect scoreDigitsDst = { scoreDigitsX, scoreY, scoreDigitsW, FH };
    const SDL_Rect copyrightDst   = { copyrightX, copyrightY, copyrightW, FH };
    const SDL_Rect gameOverDst    = { gameOverX, gameOverY, gameOverW, FH };
    const SDL_Rect pauseDst       = { pauseX, pauseY, pauseW, FH };
    const SDL_Rect gameDst        = { gameX, gameY, gameW, gameH };

    // Load SDL resources

    SDL_Surface *spritesSurface = loadThemeFile(themePath);
    if (!spritesSurface) { exit(1); }

    SDL_Surface *fontSurface = SDL_LoadBMP(XJUMP_FONTDIR "/ui-font.bmp");
    if (!fontSurface) panic("Could not load font file", SDL_GetError());

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

    SDL_Texture *font = SDL_CreateTextureFromSurface(renderer, fontSurface);
    if (!font) panic("Could not create texture", SDL_GetError());

    // At this point, everything we need is loaded to textures
    SDL_FreeSurface(spritesSurface);
    SDL_FreeSurface(fontSurface);

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

    {
        SDL_SetRenderTarget(renderer, windowBackground);

        SDL_SetRenderDrawColor(renderer, backgroundColor.r,  backgroundColor.g, backgroundColor.b, backgroundColor.a);
        SDL_RenderClear(renderer);

        draw_text_box(renderer, &titleDst);
        draw_text(renderer, font, titleMsg, textColor, &titleDst);
        draw_text(renderer, font, scoreLabelMsg, textColor, &scoreLabelDst);
        draw_text(renderer, font, copyrightMsg, copyrightColor, &copyrightDst);

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

    // FPS management (all times in milliseconds)
    uint32_t frameBudget = 0;           // Amount of time since the last simulation update
    uint32_t lastTime = SDL_GetTicks(); // Last time that we checked the clock

    while (1) {

        //
        // Respond to events
        //

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
                    switch (G.state) {
                        case STATE_RUNNING:
                            if (key.sym == SDLK_p
                             || key.sym == SDLK_PAUSE) {
                                G.state = STATE_PAUSED;
                            }
                            break;

                        case STATE_PAUSED:
                            G.state = STATE_RUNNING;
                            break;

                        case STATE_GAMEOVER:
                            G.state = STATE_HIGHSCORES;
                            break;

                        case STATE_HIGHSCORES:
                            init_input();
                            init_game(); // G.state = STATE_RUNNING;
                            break;
                    }
                    break;
                }

                case SDL_WINDOWEVENT:
                    switch (e.window.event) {
                        case SDL_WINDOWEVENT_FOCUS_LOST:
                            if (G.state == STATE_RUNNING) {
                                G.state = STATE_PAUSED;
                            }
                            break;
                    }

                default:
                    break;
            }
        }

        //
        // Update Game State
        //

        uint32_t now = SDL_GetTicks();
        frameBudget += now - lastTime;
        lastTime = now;
        while (frameBudget >= GAME_SPEED) {
            frameBudget -= GAME_SPEED;
            updateGame();
        }

        //
        // Draw
        //

        bool needsRepaint = (G.state == STATE_RUNNING || G.lastDrawn != G.state);
        if (needsRepaint) {

            SDL_SetRenderDrawColor(renderer, backgroundColor.r,  backgroundColor.g, backgroundColor.b, backgroundColor.a);
            SDL_RenderClear(renderer);
            SDL_RenderCopy(renderer, windowBackground, NULL, NULL);

            char scoreDigits[32];
            snprintf(scoreDigits, sizeof(scoreDigits), "%010ld", G.score);
            draw_text(renderer, font, scoreDigits, textColor, &scoreDigitsDst);

            if (G.state == STATE_HIGHSCORES)  {
                char line[32];
                snprintf(line, sizeof(line), "%s %6ld", highscoreMsg, best_score);

                int highscoreW = FW * strlen(line);
                int highscoreH = FH;
                int highscoreX = gameX + (gameW - highscoreW)/2;
                int highscoreY = gameY + (gameH - highscoreH)/2;

                SDL_SetRenderDrawColor(renderer, scoreBorderColor.r,  scoreBorderColor.g, scoreBorderColor.b, scoreBorderColor.a);
                SDL_RenderFillRect(renderer, &gameDst);

                const SDL_Rect innerRect = { gameX+1, gameY+1, gameW-2, gameH-2 };
                SDL_SetRenderDrawColor(renderer, backgroundColor.r,  backgroundColor.g, backgroundColor.b, backgroundColor.a);
                SDL_RenderFillRect(renderer, &innerRect);

                const SDL_Rect dst = { highscoreX, highscoreY, highscoreW, highscoreH };
                draw_text(renderer, font, line, textColor, &dst);

            } else {
                SDL_RenderSetClipRect(renderer, &gameDst);

                // Predict current hero position (without scroll)
                int hx = G.x + (G.vx/2)*((int) frameBudget)/GAME_SPEED;
                hx = max(hx, leftLimit);
                hx = min(hx, rightLimit);
                int hy = G.y + (G.vy)*((int) frameBudget)/GAME_SPEED;
                if (isStanding(hx, hy)) { hy = collideWithFloor(hy); }

                // Predict current hero position (with scroll)
                int sx = hx;
                int sy;
                if (isSoftScroll) {
                    sy = hy + G.forcedScroll + S*G.scrollCount/SCROLL_THRESHOLD;
                    sy = max(sy, topLimit);
                } else {
                    // Don't interpolate vertically, to reduce flickering during forced scrolls
                    hy = G.y;
                    sy = G.y;
                }

                // Background
                const SDL_Rect backgroundSrc = { 0, 0, backgroundW, backgroundH };
                const SDL_Rect backgroundDst = { gameX, gameY - S*FIELD_EXTRA + (sy - hy), backgroundW, backgroundH };
                SDL_RenderCopy(renderer, gameBackground, &backgroundSrc, &backgroundDst);

                // Floors
                for (int y = -FIELD_EXTRA; y < FIELD_H; y++) {
                    const Floor *floor = get_floor(G.floorOffset - y);
                    int xl = floor->left;
                    int xr = floor->right;
                    if (xl <= xr) {
                        int w = xr - xl + 1;
                        const SDL_Rect src = { 0, backgroundH, w*S, S };
                        const SDL_Rect dst = { gameX + xl*S, gameY + y*S + (sy - hy), w*S, S };
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
                if (G.state == STATE_GAMEOVER) {
                    draw_text_box(renderer, &gameOverDst);
                    draw_text(renderer, font, gameOverMsg, textColor, &gameOverDst);
                }
                if (G.state == STATE_PAUSED) {
                    draw_text_box(renderer, &pauseDst);
                    draw_text(renderer, font, pauseMsg, textColor, &pauseDst);
                }

                SDL_RenderSetClipRect(renderer, NULL);
            }

            SDL_RenderPresent(renderer);
            G.lastDrawn = G.state;

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
