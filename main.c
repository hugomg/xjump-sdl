// TODO: GPL license

#include <SDL.h>

#include <assert.h>
#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/random.h>

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

//
// Error handling
// --------------

static void panic(const char *what, const char *fullError)
{
    if (fullError == NULL) fullError = "";
    fprintf(stderr, "Fatal error! %s. %s\n", what, fullError);
    exit(1);
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

static void pcg32_init()
{
    int64_t seed[2];
    ssize_t nread = getrandom(seed, sizeof(seed), GRND_NONBLOCK);
    if (nread == -1) panic("Could not initialize RNG", strerror(errno));

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
// Joystick state
// ---------------

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

static Input translateHotkey(SDL_Keysym k)
{
    // TODO should we use scan codes?
    switch (k.sym) {
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
    // TODO should we also call this on FOCUS changes?
    K.horizDirection = LR_NEUTRAL;
    for (int i = 0; i < INPUT_OTHER; i++) {
        K.isPressing[i] = false;
    }
}

static void input_keydown(const SDL_Keysym sym)
{
    Input input = translateHotkey(sym);
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

static void input_keyup(const SDL_Keysym sym)
{
    Input input = translateHotkey(sym);
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

typedef enum {
    STATE_RUNNING,
    STATE_PAUSED,
    STATE_GAMEOVER,
} GameState;

typedef struct {
    int left;
    int right;
} Floor;

static struct {

    int64_t score;

    // Gameover / Pause
    GameState state;
    int needsRepaint;  // CPU optimization: don't redraw static screens.
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
    int hasStarted;    // Don't start scrolling until we jump for the first time
    int scrollOffset;  // Tile height of the row at the top of the screen
    int scrollCount;
    int scrollSpeed;

    // Floors
    int fpos;
    int next_floor;
    Floor floors[FIELD_H];
} G;

static Floor *get_floor(int n)
{
    return &G.floors[mod(n, FIELD_H)];
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
    G.needsRepaint  = 0;
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
    G.scrollOffset = 20;
    G.scrollCount  = 0;
    G.scrollSpeed  = 200;

    G.fpos = rnd(0,21);
    G.next_floor = -3;
    for (int i=0; i < FIELD_H; i++) {
        generate_floor();
    }
}

static void scroll()
{
    generate_floor();
    G.scrollOffset += 1;
    G.y += S;
}

static bool isStanding()
{
    if (G.vy < 0) {
        return false;
    }

    int y = (G.y + R)/S;
    if (y >= FIELD_H) {
        return false;
    }

    // We're standing as long as 8/32 pixels touch the ground.
    const Floor *fl = get_floor(G.scrollOffset - y);
    return (fl->left*S - 24 <= G.x && G.x <= fl->right*S + 8);
}

static void update_game()
{
    if (G.state == STATE_GAMEOVER) {
        G.gameOverCount++;
        return;

    }

    if (G.state == STATE_PAUSED) {
        return;
    }

    // else: state == STATE_RUNNING;

    G.needsRepaint = true;

    if (G.hasStarted) {
        if (G.scrollSpeed < 5000) {
            G.scrollSpeed++;
        }

        G.scrollCount += G.scrollSpeed;
        if (G.scrollCount > 20000) {
            G.scrollCount -= 20000;
            scroll();
        }
    }

    G.x += G.vx / 2;
    G.y += G.vy;

    if (G.y >= S*FIELD_H) {
        G.state = STATE_GAMEOVER;
        return;
    }

    if (G.y < 5*S) {
        G.scrollCount = 0;
        scroll();
    }

    // When we bounce off the walls the new X coordinate is calculated
    // assuming that we bounce back at 1/2 the speed. This is subtle but
    // makes the result of bouncing off the walls slightly more predictable.
    // The "-2" in the formula is a dampening factor that avoids a perpetual
    // 1px bounce if we run straight into a wall (which can appear as a strange
    // flickering).

    int leftLimit = S;
    if (G.x < leftLimit && G.vx <= 0) {
        G.x  = leftLimit + max(0, leftLimit - G.x - 2)/2;
        G.vx = -G.vx/2;
    }

    int rightLimit = (FIELD_W-1)*S - R;
    if (G.x > rightLimit && G.vx >= 0) {
        G.x  = rightLimit - max(0, G.x - rightLimit - 2)/2;
        G.vx = -G.vx/2;
    }

    G.isStanding = isStanding();
    if (G.isStanding) {
        G.y = (G.y/S) * S;
        G.vy = 0;

        int n = (G.scrollOffset - (G.y + R)/S) / 5;
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
}

//
// Colors
//

static const SDL_Color textColor      = { 255, 255, 255, 255 };
static const SDL_Color copyrightColor = {   0, 255,   0, 255 };
static const SDL_Color borderColor    = {   0,   0, 128, 255 };
static const SDL_Color boxColor       = {   0,   0, 255, 255 };

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

    SDL_SetRenderDrawColor(renderer, borderColor.r, borderColor.g, borderColor.b, borderColor.a);
    SDL_RenderFillRect(renderer, &border);

    SDL_SetRenderDrawColor(renderer, borderColor.r, boxColor.g, boxColor.b, boxColor.a);
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

static const int NscoreDigits = 10;

// Widths and Heights

static const char titleMsg[]      = "FALLING TOWER ver 3.0";
static const char scoreLabelMsg[] = "Floor";
static const char copyrightMsg[]  = "(C) 1997 ROYALPANDA";
static const char gameOverMsg[]   = "Game Over";
static const char pauseMsg[]      = "Pause";

static const int titleW      = FW * (sizeof(titleMsg) - 1);
static const int scoreLabelW = FW * (sizeof(scoreLabelMsg) - 1);
static const int copyrightW  = FW * (sizeof(copyrightMsg) - 1);
static const int gameOverW   = FW * (sizeof(gameOverMsg) - 1);
static const int pauseW      = FW * (sizeof(pauseMsg) - 1);

static const int textBoxH = FH + boxBorder + boxPaddingLeft + boxPaddingRight  + boxBorder;

static const int gameW = S * FIELD_W;
static const int gameH = S * FIELD_H;

static const int scoreDigitsW = (NscoreDigits + 1) * FW;
static const int scoreW = scoreLabelW + scoreDigitsW;

static const int windowW = windowMarginLeft + gameW + windowMarginRight;
static const int windowH = windowMarginTop + 3*windowMarginInner + textBoxH + 2*FH + + gameH + windowMarginBottom;

// Screen positions

static const int titleX     = (windowW - titleW)/2;
static const int scoreX     = (windowW - scoreW)/2;
static const int gameX      = (windowW - gameW)/2;
static const int copyrightX = (windowW - copyrightW)/2;

static const int titleY     = windowMarginTop + boxBorder + boxPaddingTop;
static const int scoreY     = titleY + FH + boxPaddingBottom + boxBorder + windowMarginInner;
static const int gameY      = scoreY + FH + windowMarginInner;
static const int copyrightY = gameY + gameH + windowMarginInner;

static const int scoreLabelX  = scoreX;
static const int scoreDigitsX = scoreX + scoreLabelW;

static const int gameOverX = gameX + (gameW - gameOverW)/2;
static const int gameOverY = gameY + (gameH - FH)*2/5;

static const int pauseX = gameX + (gameW - pauseW)/2;
static const int pauseY = gameY + (gameH - FH)*2/5;

static const SDL_Rect gameDst        = { gameX, gameY, gameW, gameH };
static const SDL_Rect titleDst       = { titleX, titleY, titleW, FH };
static const SDL_Rect scoreLabelDst  = { scoreLabelX, scoreY, scoreLabelW, FH };
static const SDL_Rect scoreDigitsDst = { scoreDigitsX, scoreY, scoreDigitsW, FH };
static const SDL_Rect copyrightDst   = { copyrightX, copyrightY, copyrightW, FH };
static const SDL_Rect gameOverDst    = { gameOverX, gameOverY, gameOverW, FH };
static const SDL_Rect pauseDst       = { pauseX, pauseY, pauseW, FH };

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


//
// Application state
// -----------------

int main()
{
    if (0 != SDL_Init(SDL_INIT_VIDEO)) panic("Could not initialize SDL", SDL_GetError());

    SDL_Surface *spritesSurface = SDL_LoadBMP("images/theme-jumpnbump.bmp");
    if (!spritesSurface) panic("Could not sprite file ", SDL_GetError());

    SDL_Surface *fontSurface = SDL_LoadBMP("images/ui-font.bmp");
    if (!fontSurface) panic("Could not load font file", SDL_GetError());
    SDL_SetColorKey(fontSurface, SDL_TRUE, SDL_MapRGB(fontSurface->format, 0, 0, 0)); // (make background transparent)

    SDL_Window *window = SDL_CreateWindow(
        /*title*/ "xjump",
        /*x*/ SDL_WINDOWPOS_UNDEFINED,
        /*y*/ SDL_WINDOWPOS_UNDEFINED,
        /*w*/ windowW,
        /*h*/ windowH,
        /*flags*/ SDL_WINDOW_RESIZABLE);
    if (!window) panic("Could not create window", SDL_GetError());

    SDL_Renderer *renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!renderer) panic("Coult not create SDL renderer", SDL_GetError());

    SDL_Texture *sprites = SDL_CreateTextureFromSurface(renderer, spritesSurface);
    if (!sprites) panic("Could not create texture", SDL_GetError());

    SDL_Texture *font = SDL_CreateTextureFromSurface(renderer, fontSurface);
    if (!font) panic("Could not create texture", SDL_GetError());

    // At this point, everything we need is loaded to textures
    SDL_FreeSurface(spritesSurface);
    SDL_FreeSurface(fontSurface);

    // Tell the renderer to stretch the drawing if the window is resized
    SDL_RenderSetLogicalSize(renderer, windowW, windowH);

    // Create background textures with all the things that don't change from
    // frame to frame. This reduces the number of draw calls in the inner loop.
    SDL_Texture *windowBackground = SDL_CreateTexture(
            renderer, SDL_PIXELFORMAT_RGBA8888, SDL_TEXTUREACCESS_TARGET,
            windowW, windowH);
    if (!windowBackground) panic("Could not create window background texture", SDL_GetError());

    SDL_Texture *gameBackground = SDL_CreateTexture(
            renderer, SDL_PIXELFORMAT_RGBA8888, SDL_TEXTUREACCESS_TARGET,
            gameW, gameH);
    if (!gameBackground) panic("Could not create game background texture", SDL_GetError());

    {
        SDL_SetRenderTarget(renderer, windowBackground);

        SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);;
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

        for (int y = 0; y < FIELD_H; y++) {
            for (int x = 0; x < FIELD_W; x++) {
                const SDL_Rect *src = ((x == 0) ? &LWallSprite : (x == FIELD_W-1) ? &RWallSprite : &skySprite);
                const SDL_Rect dst = { x*S, y*S, S, S };
                SDL_RenderCopy(renderer, sprites, src, &dst);
            }
        }

        SDL_RenderPresent(renderer);
        SDL_SetRenderTarget(renderer, NULL);
    }

    pcg32_init();
    init_input();
    init_game();

    while (1) {

        uint32_t frame_start_time = SDL_GetTicks();

        //
        // Respond to events
        //

        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            switch (e.type) {
                case SDL_QUIT:
                    goto quit;

                case SDL_KEYUP:
                    input_keyup(e.key.keysym);
                    break;

                case SDL_KEYDOWN:
                    input_keydown(e.key.keysym);
                    if (e.key.keysym.sym == SDLK_q && (e.key.keysym.mod & KMOD_SHIFT)) {
                        goto quit;
                    }
                    switch (G.state) {
                        case STATE_RUNNING:
                            if (e.key.keysym.sym == SDLK_p
                             || e.key.keysym.sym == SDLK_PAUSE) {
                                G.state = STATE_PAUSED;
                            }
                            break;

                        case STATE_PAUSED:
                            G.state = STATE_RUNNING;
                            break;

                        case STATE_GAMEOVER:
                            break;
                    }
                    break;

                case SDL_WINDOWEVENT:
                    if (e.window.event == SDL_WINDOWEVENT_FOCUS_LOST && G.state == STATE_RUNNING) {
                        G.state = STATE_PAUSED;
                    }
                    break;

                default:
                    break;
            }
        }

        //
        // Update Game State
        //

        update_game();
        if (G.state == STATE_GAMEOVER && G.gameOverCount >= 80) {
            goto quit; // TODO highscores
        }

        //
        // Draw
        //

        if (G.needsRepaint) {

            SDL_RenderCopy(renderer, windowBackground, NULL, NULL);
            SDL_RenderCopy(renderer, gameBackground, NULL, &gameDst);

            {
                char scoreDigits[32];
                sprintf(scoreDigits, " %010ld", G.score);
                draw_text(renderer, font, scoreDigits, textColor, &scoreDigitsDst);
            }

            {
                SDL_RenderSetClipRect(renderer, &gameDst);

                // Floors
                for (int y = 0; y < 24; y++) {
                    const Floor *fl = &G.floors[mod(G.scrollOffset - y, FIELD_H)];
                    for (int x = fl->left; x <= fl->right; x++) {
                        const SDL_Rect spriteDst = { gameX + x*S, gameY + y*S, S, S };
                        SDL_RenderCopy(renderer, sprites, &floorSprite, &spriteDst);
                    }
                }

                // Player
                int isFlying  = !G.isStanding;
                int isRight   = G.isFacingRight;
                int isVariant = (G.isStanding? G.isIdleVariant : (G.vy > 0));
                int sprite_index = (isFlying&1) << 2 | (isVariant&1) << 1 | (isRight&1) << 0;

                const SDL_Rect heroDst = { gameX + G.x, gameY + G.y, R, R };
                SDL_RenderCopy(renderer, sprites, &heroSprite[sprite_index], &heroDst);

                SDL_RenderSetClipRect(renderer, NULL);
            }

            switch (G.state) {
                case STATE_GAMEOVER:
                    draw_text_box(renderer, &gameOverDst);
                    draw_text(renderer, font, gameOverMsg, textColor, &gameOverDst);
                    G.needsRepaint = false;
                    break;

                case STATE_PAUSED:
                    draw_text_box(renderer, &pauseDst);
                    draw_text(renderer, font, pauseMsg, textColor, &pauseDst);
                    G.needsRepaint = false;
                    break;

                default:
                    break;
            }

            SDL_RenderPresent(renderer);

        }

        //
        // Wait
        //

        // SDL_Delay isn't super accurate because it is at the mercy of the OS scheduler.
        // Nevertheless, the game feels OK. TODO consider rendering at 60 FPS.
        uint32_t time_spent = SDL_GetTicks() - frame_start_time;
        uint32_t FRAME_DURATION = 25; // 40 FPS
        if (time_spent < FRAME_DURATION) {
            SDL_Delay(FRAME_DURATION - time_spent);
        }
    }

quit:

    SDL_DestroyTexture(sprites);
    SDL_DestroyTexture(font);
    SDL_DestroyTexture(gameBackground);
    SDL_DestroyTexture(windowBackground);

    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);

    SDL_Quit();
    return 0;
}
