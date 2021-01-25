#include <SDL.h>
#include <SDL_ttf.h>

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
// Input State
// -----------

// TODO: quit with Shift+Q
// TODO: pause with P.


typedef enum {
    INPUT_JUMP,
    INPUT_LEFT,
    INPUT_RIGHT,
    INPUT_QUIT,
    INPUT_PAUSE,
    INPUT_NOTHING,
} Input;

#define N_INPUT (INPUT_NOTHING+1)

static Input translateHotkey(SDL_Keysym k)
{
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

        case SDLK_q:
            if (k.mod & KMOD_SHIFT) {
                return INPUT_QUIT;
            } else {
                return INPUT_NOTHING;
            }

        case SDLK_p:
            return INPUT_PAUSE;

        default:
            return INPUT_NOTHING;
    }
}

typedef enum {
    LR_NEUTRAL,
    LR_LEFT,
    LR_RIGHT
} LeftRight;

// This struct keeps track of the "joystick" state.
// If both LEFT and RIGHT a pressed at the same time, the most recent one wins.
static struct {
    LeftRight horizDirection;
    bool isPressing[N_INPUT];
} K;

static void init_input()
{
    K.horizDirection = LR_NEUTRAL;
    for (int i=0; i < N_INPUT; i++) {
        K.isPressing[i] = false;
    }
}

static void handleKeyDown(const SDL_KeyboardEvent *e)
{
    int input = translateHotkey(e->keysym);
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

static void handleKeyUp(const SDL_KeyboardEvent *e)
{
    int input = translateHotkey(e->keysym);
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

enum GameState {
    STATE_GAME,
    STATE_PAUSE,
    STATE_HIGHSCORES,
};

typedef struct {
    int left;
    int right;
} Floor;

static struct {

    int score;

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
        floor->left  = -1;
        floor->right = -1;
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

    // TODO: one of these should be <=
    const Floor *fl = get_floor(G.scrollOffset - y);
    return (fl->left*S - 24 < G.x && G.x < fl->right*S + 8);
}

enum Alive {
    LIVE,
    DEAD,
};

static int update_game()
{
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
        printf("DEAD!\n"); // TODO: add game over screen & delete this
        return DEAD;
    }

    if (G.y < 80) {
        G.scrollCount = 0;
        scroll();
    }


    // TODO: use coordinates at the center of the sprite
    // to make this part of the code more symmetric.

    if (G.x < S && G.vx <= 0) {
        G.x  = 16;
        G.vx = -G.vx/2;
    }

    if (G.x > 29*S && G.vx >= 0) {
        G.x  = 29*S;
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

    return LIVE;
}

//
// UI Sprites
//

#define FW 15 /* Height of the UI font, in pixels */
#define FH 20 /* Width  of the UI font, in pixels */

static const SDL_Rect copyrightSprite  = {     0,   0,  19*FW,  FH };
static const SDL_Rect scoreSprite      = { 10*FW, 1*FH,  6*FW,  FH };
static const SDL_Rect digitSprites[10] = {
    { 0*FW, FH, FW, FH },
    { 1*FW, FH, FW, FH },
    { 2*FW, FH, FW, FH },
    { 3*FW, FH, FW, FH },
    { 4*FW, FH, FW, FH },
    { 5*FW, FH, FW, FH },
    { 6*FW, FH, FW, FH },
    { 7*FW, FH, FW, FH },
    { 8*FW, FH, FW, FH },
    { 9*FW, FH, FW, FH }
};
static const SDL_Rect titleSprite    = {   0, 2*FH, 330, 44 };
//static const SDL_Rect gameOverSprite = { 331, 2*FH, 150, 44 }; // TODO
//static const SDL_Rect pauseSprite    = { 482, 2*FH,  90, 44 }; // TODO


//
// Game Sprites
//

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

#define SCORE_NDIGITS 10

int main()
{
    if (0 != SDL_Init(SDL_INIT_VIDEO)) panic("Could not initialize SDL", SDL_GetError());
    if (0 != TTF_Init()) panic("Could not initialize SDL_ttf", TTF_GetError());

    // TODO resize window
    int topMargin  = 24;
    int sideMargin = 24;
    int innerMargin = 32;

    int titleW = titleSprite.w;
    int titleH = titleSprite.h;

    int scoreW = scoreSprite.w + SCORE_NDIGITS*FW;
    int scoreH = scoreSprite.h;

    int gameW = S * FIELD_W;
    int gameH = S * FIELD_H;

    int copyrightW = copyrightSprite.w;
    int copyrightH = copyrightSprite.h;

    int windowW = 2*sideMargin + gameW;
    int windowH = 2*topMargin + 3*innerMargin + titleH + scoreH + gameH + copyrightH;

    int titleX     = (windowW - titleW)/2;
    int scoreX     = (windowW - scoreW)/2;
    int gameX      = (windowW - gameW)/2;
    int copyrightX = (windowW - copyrightW)/2;

    int titleY     = topMargin;
    int scoreY     = titleY + titleH + innerMargin;
    int gameY      = scoreY + scoreH + innerMargin;
    int copyrightY = gameY + gameH + innerMargin;

    SDL_Window *window = SDL_CreateWindow(
        /*title*/ "xjump",
        /*x*/ SDL_WINDOWPOS_UNDEFINED,
        /*y*/ SDL_WINDOWPOS_UNDEFINED,
        /*w*/ windowW,
        /*h*/ windowH,
        /*flags*/ 0); // TODO: SDL_WINDOW_RESIZABLE
    if (!window) panic("Could not create window", SDL_GetError());

    SDL_Surface *windowSurface = SDL_GetWindowSurface(window);
    if (!windowSurface) panic("Could not get the window surface", SDL_GetError());

    SDL_Surface *background = SDL_ConvertSurface(windowSurface, windowSurface->format, 0);
    if (!background) panic("Could not create background surface", SDL_GetError());

    SDL_Surface *uiSprites = SDL_LoadBMP("images/ui-sprites.bmp");
    if (!uiSprites) panic("Could not load UI sprites", SDL_GetError());

    SDL_Surface *gameSprites = SDL_LoadBMP("images/theme-jumpnbump.bmp");
    if (!gameSprites) panic("Could not load game sprites", SDL_GetError());

    {
        // Draw the background elements outside of the main loop,
        // to reduce the number of draw calls in the inner loop.

        SDL_Rect titleDst = { titleX, titleY, 0, 0 };
        SDL_Rect scoreDst = { scoreX, scoreY, 0, 0 };
        SDL_Rect copyrightDst = { copyrightX, copyrightY, 0, 0 };

        SDL_FillRect(background, NULL, SDL_MapRGB(windowSurface->format, 0x00, 0x00, 0x00));
        SDL_BlitSurface(uiSprites, &titleSprite,     background, &titleDst);
        SDL_BlitSurface(uiSprites, &scoreSprite,     background, &scoreDst);
        SDL_BlitSurface(uiSprites, &copyrightSprite, background, &copyrightDst);
        for (int y = 0; y < FIELD_H; y++) {
            for (int x = 0; x < FIELD_W; x++) {
                const SDL_Rect *sprite = (
                        x == 0         ? &LWallSprite :
                        x == FIELD_W-1 ? &RWallSprite : &skySprite);
                SDL_Rect spriteDst = { gameX + x*S, gameY + y*S, 0, 0 };
                SDL_BlitSurface(gameSprites, sprite, background, &spriteDst);
            }
        }
    }

    SDL_Renderer *renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!renderer) panic("Coult not create SDL renderer", SDL_GetError());

    SDL_Texture *backgroundT = SDL_CreateTextureFromSurface(renderer, background);
    if (!backgroundT) panic("Could not create background texture", SDL_GetError());

    SDL_Texture *uiSpritesT = SDL_CreateTextureFromSurface(renderer, uiSprites);
    if (!uiSpritesT) panic("Could not create UI sprites texture", SDL_GetError());

    SDL_Texture *gameSpritesT = SDL_CreateTextureFromSurface(renderer, gameSprites);
    if (!gameSpritesT) panic("Could not create game sprites texture", SDL_GetError());

    pcg32_init();
    init_input();
    init_game();
    int live = LIVE;

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

                case SDL_KEYDOWN:
                    handleKeyDown(&e.key);
                    break;

                case SDL_KEYUP:
                    handleKeyUp(&e.key);
                    break;

                default:
                    break;
            }
        }

        //
        // Update Game State
        //

        if (K.isPressing[INPUT_QUIT]) {
            goto quit;
        }

        if (live == LIVE) {
            live = update_game();
        }

        //
        // Draw Screen
        //

        SDL_RenderClear(renderer);
        SDL_RenderCopy(renderer, backgroundT, NULL, NULL);

        {
            int digits[SCORE_NDIGITS];

            long int s = G.score;
            for (int i = SCORE_NDIGITS-1; i >= 0; i--) {
                digits[i] = s % 10;
                s = s / 10;
            }

            for (int i = 0; i < SCORE_NDIGITS; i++) {
                int d = digits[i];
                SDL_Rect digitDst = { scoreX + scoreSprite.w + i*FW, scoreY, FW, FH };
                SDL_RenderCopy(renderer, uiSpritesT, &digitSprites[d], &digitDst);
            }
        }

        SDL_Rect clipRect  = { gameX, gameY, gameW, gameH };
        SDL_RenderSetClipRect(renderer, &clipRect);
        {
            for (int y = 0; y < 24; y++) {
                const Floor *fl = &G.floors[mod(G.scrollOffset - y, FIELD_H)];
                if (fl->left > 0) {
                    for (int x = fl->left; x <= fl->right; x++) {
                        SDL_Rect spriteDst = { gameX + x*S, gameY + y*S, S, S };
                        SDL_RenderCopy(renderer, gameSpritesT, &floorSprite, &spriteDst);
                    }
                }
            }

            int isFlying  = !G.isStanding;
            int isRight   = G.isFacingRight;
            int isVariant = (G.isStanding? G.isIdleVariant : (G.vy > 0));
            int sprite_index = ((isFlying&1) << 2) | ((isVariant&1) << 1) | ((isRight&1) << 0);

            SDL_Rect heroDst = { gameX + G.x, gameY + G.y, R, R };
            SDL_RenderCopy(renderer, gameSpritesT, &heroSprite[sprite_index], &heroDst);
        }
        SDL_RenderSetClipRect(renderer, NULL);
        SDL_RenderPresent(renderer);

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

    //
    // Cleanup
    //

    SDL_DestroyTexture(gameSpritesT);
    SDL_DestroyTexture(uiSpritesT);
    SDL_DestroyTexture(backgroundT);
    SDL_DestroyRenderer(renderer);

    SDL_FreeSurface(gameSprites);
    SDL_FreeSurface(uiSprites);
    SDL_FreeSurface(background);
    SDL_DestroyWindow(window);

    TTF_Quit();
    SDL_Quit();

    return 0;
}
