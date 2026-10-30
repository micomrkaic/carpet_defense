/* ====================================================================
   housekeeper.c  –  "The Housekeeper"   (Space Invaders variant)
   C17 + SDL2.  Identical native and WASM builds.

   NATIVE:  make            (gcc, sdl2-config)
   WASM:    make wasm       (emcc -> docs/)
   See housekeeper(6) for options and controls.

   Portability: all state in one global struct, frame = main_loop().
   The only __EMSCRIPTEN__ blocks are the main-loop driver in main()
   and the two hi-score storage functions (file vs localStorage).
   ==================================================================== */

#ifdef __EMSCRIPTEN__
#  include <emscripten.h>
#else
#  include <sys/stat.h>
#endif
#include <SDL2/SDL.h>
#include <math.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define HK_VERSION "1.2.0"

/* ─── screen & layout ─────────────────────────────────────── */
#define SW         900
#define SH         650
#define HUD_H       40
#define CARPET_Y   550
#define UFO_Y      (HUD_H + 22)      /* Bloody Mary lane */

/* ─── invader grid ────────────────────────────────────────── */
#define GCOLS       10
#define GROWS        4
#define CW          56
#define CH          46
#define GX         ((SW - GCOLS*CW)/2)
#define GY         (HUD_H + 44)      /* leaves room for the flyer lane */

/* ─── gameplay constants ──────────────────────────────────── */
#define MAX_PROJ    64
#define MAX_SPLATS 200
#define MAX_EX      32
#define N_BARS       4
#define BAR_HP_MAX   4
#define BAR_STAINS   8
#define MAX_SOIL    30
#define INIT_LIVES   3
#define MAX_LIVES    5
#define PLAYER_SPD 280.f
#define BULLET_SPD 480.f
#define DROP_SPD   155.f
#define UFO_SPD    110.f
#define MIN_MS     100u
#define WAVE_MIN_MS 300u
#define MOP_TIME   0.9f
#define MOP_REACH  14.f
#define N_DIFF       4

/* ─── game states ─────────────────────────────────────────── */
#define ST_SELECT   2
#define ST_PLAY     0
#define ST_WAVE     3      /* wave cleared, short intermission */
#define ST_LOSE    (-1)

#define RGB(r,gv,b) SDL_SetRenderDrawColor(ren,(r),(gv),(b),255)
#define RGBA(r,gv,b,a) SDL_SetRenderDrawColor(ren,(r),(gv),(b),(a))

/* ====================================================================
   DIFFICULTY TABLE
   ==================================================================== */
typedef struct {
    const char *name;
    const char *desc[2];
    int         max_bullets;
    Uint32      base_ms;
    Uint32      reload_ms;
} DiffRow;

static const DiffRow DIFFS[N_DIFF] = {
    { "EASY",      {"3 BULLETS SIMULTANEOUSLY", "SLOW INVADERS - NO RELOAD WAIT"}, 3, 1200,    0 },
    { "MEDIUM",    {"2 BULLETS AT ONCE",        "250MS RELOAD BETWEEN SHOTS"},     2,  900,  250 },
    { "HARD",      {"1 BULLET AT A TIME",       "600MS RELOAD BETWEEN SHOTS"},     1,  900,  600 },
    { "NIGHTMARE", {"1 BULLET AT A TIME",       "FAST INVADERS - 1.2S RELOAD"},    1,  650, 1200 },
};

static const SDL_Color DIFF_COL[N_DIFF] = {
    {0x30,0xD0,0x60,255}, {0x40,0xA0,0xFF,255}, {0xFF,0xA0,0x20,255}, {0xFF,0x30,0x30,255},
};

/* ====================================================================
   AUDIO  –  procedural 8-bit SFX via SDL_QueueAudio (no callback)
   ==================================================================== */
#define SR        22050
#define MAX_AMP   12000
#define SCRATCH_S (SR * 4)

typedef enum {
    SFX_SHOOT = 0, SFX_FAT, SFX_DRIP, SFX_LIFE, SFX_START, SFX_WIN, SFX_LOSE,
    SFX_BONUS, SFX_MOP, SFX_BLOODY,
    SFX_COUNT
} SfxId;

typedef struct { int16_t *buf; int n; } Sfx;
static Sfx               sfx[SFX_COUNT];
static SDL_AudioDeviceID audio_dev = 0;
static int               muted     = 0;

static int seg_sq(int16_t *b, float f0, float f1, int ms, int16_t amp) {
    int n = SR * ms / 1000; float ph = 0.f;
    for (int i = 0; i < n; i++) {
        float t = (float)i / (float)n, f = f0 + (f1 - f0) * t;
        ph += f / (float)SR; if (ph >= 1.f) ph -= 1.f;
        b[i] = (int16_t)((float)amp * (1.f - t * 0.75f) * (ph < 0.5f ? 1.f : -1.f));
    }
    return n;
}
static int seg_noise(int16_t *b, int ms, int16_t amp) {
    int n = SR * ms / 1000; uint32_t s = 0xDEAD1234u;
    for (int i = 0; i < n; i++) {
        float t = (float)i / (float)n;
        s ^= s << 13; s ^= s >> 17; s ^= s << 5;
        int32_t v = (int32_t)(s >> 16) - 32768;
        b[i] = (int16_t)((int32_t)((float)amp * (1.f - t)) * v / 32767);
    }
    return n;
}
static int seg_sil(int16_t *b, int ms) {
    int n = SR * ms / 1000; memset(b, 0, (size_t)n * sizeof(int16_t)); return n;
}
static void bake(int16_t *tmp, int n, SfxId id) {
    sfx[id].n = n; sfx[id].buf = malloc((size_t)n * sizeof(int16_t));
    if (sfx[id].buf) memcpy(sfx[id].buf, tmp, (size_t)n * sizeof(int16_t));
}
static int seg_notes(int16_t *t, const float *nf, const int *nd, int cnt, float glide, int gap) {
    int n = 0;
    for (int i = 0; i < cnt; i++) {
        n += seg_sq(t+n, nf[i], nf[i]*glide, nd[i], MAX_AMP);
        if (gap) n += seg_sil(t+n, gap);
    }
    return n;
}

static void init_audio(void) {
    SDL_AudioSpec want = { .freq=SR, .format=AUDIO_S16SYS, .channels=1, .samples=512, .callback=NULL }, got;
    audio_dev = SDL_OpenAudioDevice(NULL, 0, &want, &got, 0);
    if (!audio_dev) { SDL_Log("Audio unavailable: %s", SDL_GetError()); return; }
    SDL_PauseAudioDevice(audio_dev, 0);

    int16_t *t = malloc((size_t)SCRATCH_S * sizeof(int16_t));
    if (!t) { SDL_CloseAudioDevice(audio_dev); audio_dev = 0; return; }
    int n;

    n = seg_sq(t, 620, 110, 65, MAX_AMP);                            bake(t, n, SFX_SHOOT);
    n = seg_noise(t, 55, MAX_AMP); n += seg_sq(t+n, 110, 45, 90, MAX_AMP*3/4); bake(t, n, SFX_FAT);
    n = seg_sq(t, 520, 220, 45, MAX_AMP); n += seg_sq(t+n, 220, 90, 55, MAX_AMP/2); bake(t, n, SFX_DRIP);
    { static const float nf[]={440,330,220,165}; static const int nd[]={100,110,130,200};
      bake(t, seg_notes(t,nf,nd,4,0.78f,0), SFX_LIFE); }
    { static const float nf[]={262,330,392,523,659,784}; static const int nd[]={70,70,70,70,70,280};
      bake(t, seg_notes(t,nf,nd,6,1.f,12), SFX_START); }
    { static const float nf[]={523,659,784,659,784,1047}; static const int nd[]={90,90,90,90,90,380};
      bake(t, seg_notes(t,nf,nd,6,1.f,18), SFX_WIN); }
    { static const float nf[]={392,330,262,196}; static const int nd[]={140,180,240,480};
      bake(t, seg_notes(t,nf,nd,4,0.68f,0), SFX_LOSE); }
    /* bonus: fast ascending trill */
    { static const float nf[]={880,1109,1319,1760}; static const int nd[]={45,45,45,160};
      bake(t, seg_notes(t,nf,nd,4,1.f,8), SFX_BONUS); }
    /* mop: wet swish */
    n = seg_noise(t, 140, MAX_AMP/2); n += seg_sq(t+n, 300, 600, 60, MAX_AMP/3); bake(t, n, SFX_MOP);
    /* bloody mary splash: heavy noise + low thud */
    n = seg_noise(t, 110, MAX_AMP); n += seg_sq(t+n, 90, 40, 160, MAX_AMP); bake(t, n, SFX_BLOODY);

    free(t);
}

static void play_sfx(SfxId id, int priority) {
    if (muted || !audio_dev || !sfx[id].buf) return;
    if (priority) SDL_ClearQueuedAudio(audio_dev);
    else if (SDL_GetQueuedAudioSize(audio_dev) > (Uint32)(SR * sizeof(int16_t) / 4)) return;
    SDL_QueueAudio(audio_dev, sfx[id].buf, (Uint32)((size_t)sfx[id].n * sizeof(int16_t)));
}
static void free_audio(void) {
    if (audio_dev) { SDL_CloseAudioDevice(audio_dev); audio_dev = 0; }
    for (int i = 0; i < SFX_COUNT; i++) { free(sfx[i].buf); sfx[i].buf = NULL; }
}

/* ====================================================================
   TYPES
   ==================================================================== */
typedef enum { T_BURGER, T_MARTINI, T_BLOODY } InvT;   /* T_BLOODY: drops/splats only */

typedef struct { float x, y; int alive; InvT type; } Invader;
typedef struct { float x, y, vy; int active, is_bullet; InvT dtype; } Proj;
typedef struct { int dx, dy; InvT t; } Stain;
typedef struct { int x, y, hp, nst; Stain st[BAR_STAINS]; } Bar;
typedef struct { int x, y; InvT type; } Splat;
typedef struct { int active; float x, y, t; InvT type; int pts; } Explosion;
typedef struct { int active, dropped, worth; float x, vx, drop_x, timer; } Ufo;

typedef struct {
    SDL_Window         *win;
    SDL_Renderer       *ren;
    SDL_GameController *pad;

    int    difficulty, sel_diff;
    int    hi[N_DIFF];

    int    max_bullets;
    Uint32 base_step_ms, reload_ms, last_shot;

    Invader inv[GROWS][GCOLS];
    int     alive_count, anim;
    float   gox, goy;
    int     gdir;
    Uint32  last_step, step_ms;

    Proj      proj[MAX_PROJ];
    Bar       bar[N_BARS];
    Splat     splat[MAX_SPLATS];
    int       n_splat;
    Explosion ex[MAX_EX];
    Ufo       ufo;

    float  px, py;
    int    pdead, lives, score, wave;
    int    mopping; float mop_t;
    float  banner_t, wave_t;
    int    new_hi, wave_life;

    int    state, paused, running;
    Uint32 prev;
} G;

static G g;

/* ====================================================================
   HI-SCORE STORAGE  (the only other platform split in the file)
   ==================================================================== */
#ifdef __EMSCRIPTEN__
/* plain function calls, not EM_ASM: EM_ASM needs -std=gnu*, we stay strict C17 */
static void hi_load(void) {
    char js[200];
    for (int i = 0; i < N_DIFF; i++) {
        snprintf(js, sizeof js,
            "(function(){try{return parseInt(localStorage.getItem('hk_hi%d')||'0',10)|0;}"
            "catch(e){return 0;}})()", i);
        g.hi[i] = emscripten_run_script_int(js);
    }
}
static void hi_save(void) {
    char js[200];
    for (int i = 0; i < N_DIFF; i++) {
        snprintf(js, sizeof js,
            "try{localStorage.setItem('hk_hi%d','%d');}catch(e){}", i, g.hi[i]);
        emscripten_run_script(js);
    }
}
#else
static const char *hi_path(char *buf, size_t n) {
    const char *xdg = getenv("XDG_STATE_HOME"), *home = getenv("HOME");
    if (xdg && *xdg) snprintf(buf, n, "%s/housekeeper", xdg);
    else if (home)   snprintf(buf, n, "%s/.local/state/housekeeper", home);
    else return NULL;
    return buf;
}
static void hi_load(void) {
    char dir[512], path[600];
    if (!hi_path(dir, sizeof dir)) return;
    snprintf(path, sizeof path, "%s/hiscore", dir);
    FILE *f = fopen(path, "r");
    if (!f) return;
    for (int i = 0; i < N_DIFF; i++) if (fscanf(f, "%d", &g.hi[i]) != 1) g.hi[i] = 0;
    fclose(f);
}
static void hi_save(void) {
    char dir[512], path[600], parent[512];
    if (!hi_path(dir, sizeof dir)) return;
    /* mkdir -p for two levels: .../.local/state and .../housekeeper */
    snprintf(parent, sizeof parent, "%s", dir);
    char *slash = strrchr(parent, '/'); if (slash) { *slash = 0; mkdir(parent, 0700); }
    mkdir(dir, 0700);
    snprintf(path, sizeof path, "%s/hiscore", dir);
    FILE *f = fopen(path, "w");
    if (!f) return;
    for (int i = 0; i < N_DIFF; i++) fprintf(f, "%d%c", g.hi[i], i == N_DIFF-1 ? '\n' : ' ');
    fclose(f);
}
#endif

static void commit_hiscore(void) {
    if (g.score > g.hi[g.difficulty]) { g.hi[g.difficulty] = g.score; g.new_hi = 1; hi_save(); }
}

/* ====================================================================
   5×7 BITMAP FONT  (ASCII 0x20..0x5A)
   ==================================================================== */
static const uint8_t FONT[0x5B][7] = {
[0x20]={0,0,0,0,0,0,0},          [0x21]={4,4,4,4,0,4,0},          [0x2B]={0,4,4,31,4,4,0},
[0x2D]={0,0,31,0,0,0,0},         [0x2E]={0,0,0,0,0,12,12},        [0x3A]={0,12,12,0,12,12,0},
[0x30]={14,17,19,21,25,17,14},   [0x31]={4,12,4,4,4,4,14},        [0x32]={14,17,1,6,8,16,31},
[0x33]={31,2,4,2,1,17,14},       [0x34]={2,6,10,18,31,2,2},       [0x35]={31,16,30,1,1,17,14},
[0x36]={6,8,16,30,17,17,14},     [0x37]={31,1,2,4,8,8,8},         [0x38]={14,17,17,14,17,17,14},
[0x39]={14,17,17,15,1,2,12},
[0x41]={4,10,17,17,31,17,17},    [0x42]={30,17,17,30,17,17,30},   [0x43]={14,17,16,16,16,17,14},
[0x44]={28,18,17,17,17,18,28},   [0x45]={31,16,16,30,16,16,31},   [0x46]={31,16,16,30,16,16,16},
[0x47]={14,17,16,23,17,17,15},   [0x48]={17,17,17,31,17,17,17},   [0x49]={14,4,4,4,4,4,14},
[0x4A]={7,2,2,2,2,18,12},        [0x4B]={17,18,20,24,20,18,17},   [0x4C]={16,16,16,16,16,16,31},
[0x4D]={17,27,21,17,17,17,17},   [0x4E]={17,25,21,19,17,17,17},   [0x4F]={14,17,17,17,17,17,14},
[0x50]={30,17,17,30,16,16,16},   [0x51]={14,17,17,17,21,18,13},   [0x52]={30,17,17,30,20,18,17},
[0x53]={14,17,16,14,1,17,14},    [0x54]={31,4,4,4,4,4,4},         [0x55]={17,17,17,17,17,17,14},
[0x56]={17,17,17,17,17,10,4},    [0x57]={17,17,17,21,21,27,17},   [0x58]={17,17,10,4,10,17,17},
[0x59]={17,17,10,4,4,4,4},       [0x5A]={31,1,2,4,8,16,31},
};

/* ====================================================================
   DRAWING PRIMITIVES
   ==================================================================== */
static void fill_circle(SDL_Renderer *ren, int cx, int cy, int r) {
    for (int dy = -r; dy <= r; dy++) {
        int dx = (int)sqrtf((float)(r*r - dy*dy));
        SDL_RenderDrawLine(ren, cx-dx, cy+dy, cx+dx, cy+dy);
    }
}
static void ring(SDL_Renderer *ren, int cx, int cy, int r) {
    for (int a = 0; a < 32; a++) {
        float rad = (float)a * 6.2831853f / 32.f;
        SDL_Rect p = { cx + (int)(r*cosf(rad)) - 1, cy + (int)(r*sinf(rad)) - 1, 2, 2 };
        SDL_RenderFillRect(ren, &p);
    }
}
static void draw_char(SDL_Renderer *ren, int x, int y, uint8_t c, int s) {
    if (c < 0x20 || c >= 0x5B) c = 0x20;
    const uint8_t *row = FONT[c];
    for (int r = 0; r < 7; r++)
        for (int co = 0; co < 5; co++)
            if (row[r] & (uint8_t)(0x10u >> co)) {
                SDL_Rect p = {x + co*s, y + r*s, s, s}; SDL_RenderFillRect(ren, &p);
            }
}
static void draw_str(SDL_Renderer *ren, int x, int y, const char *s, int sc) {
    for (; *s; s++, x += 6*sc) {
        uint8_t c = (uint8_t)*s;
        if (c >= 'a' && c <= 'z') c = (uint8_t)(c - 32);
        draw_char(ren, x, y, c, sc);
    }
}
static void draw_strf(SDL_Renderer *ren, int x, int y, int sc, const char *fmt, ...) {
    char buf[64]; va_list ap; va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap); va_end(ap);
    draw_str(ren, x, y, buf, sc);
}
static int  str_w(const char *s, int sc) { return (int)strlen(s) * 6 * sc; }
static void draw_cx(SDL_Renderer *ren, int cx, int y, const char *s, int sc) {
    draw_str(ren, cx - str_w(s,sc)/2, y, s, sc);
}
static void draw_cxf(SDL_Renderer *ren, int cx, int y, int sc, const char *fmt, ...) {
    char buf[64]; va_list ap; va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap); va_end(ap);
    draw_cx(ren, cx, y, buf, sc);
}

/* ====================================================================
   ENTITY DRAWING
   ==================================================================== */
/* frame: 0/1 alternates on every grid step */
static void draw_burger(SDL_Renderer *ren, int cx, int cy, int frame) {
    int bob = frame ? -1 : 0;                   /* top bun lifts 1px */
    RGB(0xD4,0x7C,0x1E);
    for (int dy=-11; dy<=0; dy++) {
        float t=(float)(dy+11)/11.f; int hw=(int)(16.f*sqrtf(t));
        SDL_RenderDrawLine(ren,cx-hw,cy+dy+bob,cx+hw,cy+dy+bob);
    }
    RGB(0xF5,0xEE,0xD0);
    SDL_Rect sd1={cx-7,cy-9+bob,3,2},sd2={cx+3,cy-10+bob,3,2},sd3={cx-2,cy-10+bob,3,2};
    SDL_RenderFillRect(ren,&sd1);SDL_RenderFillRect(ren,&sd2);SDL_RenderFillRect(ren,&sd3);
    RGB(0xF0,0xCC,0x30); SDL_Rect ch={cx-15,cy+1,30,5}; SDL_RenderFillRect(ren,&ch);
    RGB(0xD0,0xA8,0x20); SDL_Rect fold={frame?cx+9:cx-15,cy+1,6,5}; SDL_RenderFillRect(ren,&fold);
    RGB(0x52,0x26,0x06); SDL_Rect pt={cx-14,cy+6,28,7}; SDL_RenderFillRect(ren,&pt);
    RGB(0x70,0x38,0x10); SDL_Rect ph={cx-12,cy+7,10,2}; SDL_RenderFillRect(ren,&ph);
    RGB(0x22,0x88,0x22);
    for (int i=-14;i<=10;i+=4){int z=(((i/4)+frame)&1)?2:0;SDL_Rect lt={cx+i,cy+13+z,4,3};SDL_RenderFillRect(ren,&lt);}
    RGB(0xB8,0x64,0x10);
    for (int dy=0;dy<=7;dy++){float t=(float)(7-dy)/7.f;int hw=(int)(14.f*sqrtf(1.f-t*t)+2);SDL_RenderDrawLine(ren,cx-hw,cy+16+dy,cx+hw,cy+16+dy);}
}

static void draw_martini(SDL_Renderer *ren, int cx, int cy, int frame) {
    const int TW=17; int tilt = frame ? 1 : -1;   /* liquid sloshes */
    for (int dy=0;dy<=20;dy++){
        float t=(float)dy/20.f; int hw=(int)(TW*(1.f-t));
        if(dy<3){RGB(0x70,0xE0,0xFF);}else{RGB(0x28,0xA0,0xD0);}
        int sl = dy<4 ? tilt : 0;
        SDL_RenderDrawLine(ren,cx-hw+sl,cy-13+dy,cx+hw+sl,cy-13+dy);
    }
    RGB(0xC0,0xF0,0xFF);
    SDL_RenderDrawLine(ren,cx-TW,cy-13,cx,cy+7); SDL_RenderDrawLine(ren,cx+TW,cy-13,cx,cy+7);
    SDL_RenderDrawLine(ren,cx-TW,cy-13,cx+TW,cy-13);
    RGB(0xB0,0xE8,0xF8);
    SDL_RenderDrawLine(ren,cx,cy+7,cx,cy+17); SDL_RenderDrawLine(ren,cx-9,cy+17,cx+9,cy+17);
    int ox = frame ? cx-2 : cx-4, oy = frame ? cy-7 : cy-8;
    RGB(0x30,0x90,0x20); fill_circle(ren,ox,oy,3);
    RGB(0xE0,0x30,0x30); fill_circle(ren,ox,oy,1);
    RGB(0xFF,0xCC,0x40); SDL_RenderDrawLine(ren,ox,oy-3,ox+10,oy+3);
}

/* Bloody Mary: highball glass, celery, lemon, straw (~30x30) */
static void draw_bloody(SDL_Renderer *ren, int cx, int cy) {
    RGB(0xC0,0x18,0x18); SDL_Rect liq={cx-8,cy-6,16,17}; SDL_RenderFillRect(ren,&liq);
    RGB(0xE8,0x40,0x30); SDL_Rect hi={cx-6,cy-6,3,14}; SDL_RenderFillRect(ren,&hi);
    RGB(0xD8,0xF0,0xFF);
    SDL_RenderDrawLine(ren,cx-9,cy-10,cx-9,cy+12); SDL_RenderDrawLine(ren,cx+9,cy-10,cx+9,cy+12);
    SDL_RenderDrawLine(ren,cx-9,cy+12,cx+9,cy+12); SDL_RenderDrawLine(ren,cx-9,cy-10,cx+9,cy-10);
    RGB(0x50,0xB0,0x30); SDL_Rect cel={cx+3,cy-18,3,14}; SDL_RenderFillRect(ren,&cel);   /* celery */
    RGB(0x80,0xD0,0x50); SDL_RenderDrawLine(ren,cx+4,cy-18,cx+2,cy-22); SDL_RenderDrawLine(ren,cx+5,cy-18,cx+8,cy-21);
    RGB(0xF0,0xE0,0x40); fill_circle(ren,cx-9,cy-10,4);                                   /* lemon */
    RGB(0xFF,0xF8,0xA0); fill_circle(ren,cx-9,cy-10,2);
    RGB(0xFF,0xFF,0xFF); SDL_RenderDrawLine(ren,cx-3,cy-9,cx-6,cy-20);                   /* straw */
    RGB(0xE0,0x30,0x30); SDL_RenderDrawLine(ren,cx-4,cy-9,cx-7,cy-20);
}

static void draw_housekeeper(SDL_Renderer *ren, int cx, int cy) {
    if (g.pdead > 0 && (g.pdead/4)%2 == 0) return;
    RGB(0x18,0x18,0x28); SDL_Rect body={cx-9,cy-10,18,24}; SDL_RenderFillRect(ren,&body);
    RGB(0xF0,0xF0,0xF0);
    SDL_Rect bib={cx-4,cy-12,8,8}; SDL_RenderFillRect(ren,&bib);
    SDL_Rect skirt={cx-5,cy-2,10,14}; SDL_RenderFillRect(ren,&skirt);
    RGB(0xF8,0xD0,0xD0);
    SDL_Rect b1={cx-7,cy-3,5,4},b2={cx+2,cy-3,5,4}; SDL_RenderFillRect(ren,&b1); SDL_RenderFillRect(ren,&b2);
    RGB(0xE8,0xC8,0xA0); fill_circle(ren,cx,cy-18,7);
    RGB(0xF0,0xF0,0xF0); SDL_Rect cap={cx-7,cy-27,14,10}; SDL_RenderFillRect(ren,&cap);
    RGB(0x18,0x18,0x28); SDL_Rect cb={cx-7,cy-19,14,3}; SDL_RenderFillRect(ren,&cb);
    RGB(0x10,0x10,0x10);
    SDL_Rect el={cx-4,cy-20,2,2},er={cx+2,cy-20,2,2}; SDL_RenderFillRect(ren,&el); SDL_RenderFillRect(ren,&er);
    if (g.mopping) {
        /* mop: handle from right hand down to the floor at left */
        RGB(0xA0,0x70,0x30); SDL_RenderDrawLine(ren,cx+8,cy-6,cx-16,cy+14);
        RGB(0xD0,0xD0,0xD0); fill_circle(ren,cx-17,cy+15,4);
        RGB(0xA8,0xA8,0xB8); SDL_RenderDrawLine(ren,cx-21,cy+17,cx-13,cy+17);
        RGB(0xE8,0xC8,0xA0); SDL_RenderDrawLine(ren,cx+8,cy-4,cx+8,cy-6);
        SDL_RenderDrawLine(ren,cx-8,cy-4,cx-6,cy+3);
        /* progress bar above head */
        if (g.mop_t > 0.f) {
            int w = (int)(28.f * g.mop_t / MOP_TIME);
            RGB(0x28,0x28,0x28); SDL_Rect bg={cx-14,cy-34,28,3}; SDL_RenderFillRect(ren,&bg);
            RGB(0x60,0xE0,0xFF); SDL_Rect fg={cx-14,cy-34,w,3};  SDL_RenderFillRect(ren,&fg);
        }
    } else {
        RGB(0x80,0xC0,0xE0); SDL_Rect sb={cx+9,cy-8,5,10}; SDL_RenderFillRect(ren,&sb);
        RGB(0xA0,0xD8,0xF0); SDL_Rect st={cx+10,cy-14,3,7}; SDL_RenderFillRect(ren,&st);
        RGB(0x60,0xA0,0xC0); SDL_RenderDrawLine(ren,cx+11,cy-14,cx+16,cy-19);
        RGB(0xE8,0xC8,0xA0);
        SDL_RenderDrawLine(ren,cx+8,cy-4,cx+9,cy-8); SDL_RenderDrawLine(ren,cx-8,cy-4,cx-12,cy+2);
    }
}

static void splat_color(SDL_Renderer *ren, InvT t) {
    if      (t==T_BURGER)  { RGB(0xA8,0x88,0x10); }
    else if (t==T_MARTINI) { RGB(0x18,0x88,0xB0); }
    else                   { RGB(0xB0,0x18,0x18); }
}

static void draw_barricade(SDL_Renderer *ren, const Bar *b) {
    int cx=b->x, cy=b->y, hp=b->hp;
    if (hp<=0) return;
    const int BW=36,BH=18;
    RGB(0xC8,0x82,0x30); SDL_Rect body={cx-BW,cy-BH/2,BW*2,BH}; SDL_RenderFillRect(ren,&body);
    RGB(0xA0,0x66,0x20);
    SDL_RenderDrawLine(ren,cx-BW,cy-3,cx+BW,cy-3); SDL_RenderDrawLine(ren,cx-BW,cy+3,cx+BW,cy+3);
    SDL_RenderDrawLine(ren,cx,cy-BH/2,cx,cy+BH/2);
    RGB(0xE8,0xD0,0x70);
    SDL_Rect p1={cx-16,cy-2,4,4},p2={cx+12,cy-2,4,4}; SDL_RenderFillRect(ren,&p1); SDL_RenderFillRect(ren,&p2);
    if (hp>=2){RGB(0xA8,0x68,0x24);int ly=cy+BH/2;SDL_RenderDrawLine(ren,cx-BW+5,ly,cx-BW+3,ly+14);SDL_RenderDrawLine(ren,cx+BW-5,ly,cx+BW-3,ly+14);}
    if (hp>=3){RGB(0xA8,0x68,0x24);int ly=cy+BH/2;SDL_RenderDrawLine(ren,cx-10,ly,cx-11,ly+14);SDL_RenderDrawLine(ren,cx+10,ly,cx+11,ly+14);}
    /* stains from absorbed drops */
    for (int i=0;i<b->nst;i++){ splat_color(ren,b->st[i].t); fill_circle(ren,cx+b->st[i].dx,cy+b->st[i].dy,3); }
    SDL_SetRenderDrawColor(ren,0x0A,0x0A,0x14,255);
    if (hp<=2){SDL_Rect n1={cx-BW,cy-BH/2,12,9}; SDL_RenderFillRect(ren,&n1);}
    if (hp==1){SDL_Rect n2={cx+BW-12,cy+1,12,8};  SDL_RenderFillRect(ren,&n2);}
}

static void draw_bullet(SDL_Renderer *ren, int cx, int cy) {
    RGB(0xFF,0xFF,0x60); SDL_Rect r={cx-2,cy-6,4,12}; SDL_RenderFillRect(ren,&r);
    RGB(0xFF,0xFF,0xFF); SDL_Rect hi={cx-1,cy-6,2,4}; SDL_RenderFillRect(ren,&hi);
}
static void draw_drop(SDL_Renderer *ren, int cx, int cy, InvT t) {
    if (t==T_BURGER) {
        RGB(0xD8,0xB8,0x18); fill_circle(ren,cx,cy,5);
        RGB(0xFF,0xE8,0x60); fill_circle(ren,cx-1,cy-1,2);
    } else if (t==T_MARTINI) {
        RGB(0x20,0xB8,0xE0); fill_circle(ren,cx,cy,4);
        RGB(0x80,0xE8,0xFF); fill_circle(ren,cx,cy-2,2);
        RGB(0x20,0xB8,0xE0); SDL_RenderDrawLine(ren,cx,cy+4,cx,cy+8);
    } else {
        RGB(0xC0,0x18,0x18); fill_circle(ren,cx,cy,7);
        RGB(0xE8,0x50,0x40); fill_circle(ren,cx-2,cy-2,3);
        RGB(0xC0,0x18,0x18); SDL_RenderDrawLine(ren,cx,cy+6,cx,cy+12);
    }
}
static void draw_splat(SDL_Renderer *ren, int sx, int sy, InvT type) {
    splat_color(ren,type);
    fill_circle(ren,sx,sy,5);
    for (int a=0;a<360;a+=60){float rad=(float)a*3.14159f/180.f;fill_circle(ren,sx+(int)(8.f*cosf(rad)),sy+(int)(5.f*sinf(rad)),2);}
}
static void draw_explosion(SDL_Renderer *ren, const Explosion *e) {
    float k = e->t / 0.4f;                         /* 0..1 */
    int   r = 4 + (int)(34.f * k);
    Uint8 a = (Uint8)(255.f * (1.f - k));
    SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_BLEND);
    if      (e->type==T_BURGER)  { RGBA(0xFF,0xC0,0x40,a); }
    else if (e->type==T_MARTINI) { RGBA(0x80,0xE8,0xFF,a); }
    else                         { RGBA(0xFF,0x50,0x50,a); }
    ring(ren,(int)e->x,(int)e->y,r);
    for (int i=0;i<8;i++) {                         /* flying bits */
        float rad=(float)i*0.7854f + 0.3f, d=10.f+60.f*k;
        SDL_Rect p={(int)(e->x+d*cosf(rad))-1,(int)(e->y+d*sinf(rad)-20.f*k*k)-1,3,3};
        SDL_RenderFillRect(ren,&p);
    }
    if (e->pts>0) { RGBA(0xFF,0xFF,0xFF,a); draw_cxf(ren,(int)e->x,(int)(e->y-16.f-30.f*k),1,"+%d",e->pts); }
    SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_NONE);
}
static void draw_soil_meter(SDL_Renderer *ren) {
    int pct=(g.n_splat*100)/MAX_SOIL; if(pct>100)pct=100;
    int bh=SH-HUD_H-20, filled=bh*pct/100;
    RGB(0x18,0x18,0x18); SDL_Rect bg={8,HUD_H+10,14,bh}; SDL_RenderFillRect(ren,&bg);
    if(pct<50){RGB(0x30,0xA8,0x30);}else if(pct<80){RGB(0xD0,0xA0,0x20);}else{RGB(0xD0,0x30,0x30);}
    if(filled>0){SDL_Rect f={9,HUD_H+10+bh-filled,12,filled};SDL_RenderFillRect(ren,&f);}
    RGB(0x80,0x80,0x80); SDL_RenderDrawRect(ren,&bg);
    draw_str(ren,6,HUD_H+bh+14,"SOIL",1);
}

/* ====================================================================
   GAME SETUP
   ==================================================================== */
static void add_explosion(float x, float y, InvT t, int pts) {
    for (int i=0;i<MAX_EX;i++) if (!g.ex[i].active) {
        g.ex[i]=(Explosion){.active=1,.x=x,.y=y,.t=0.f,.type=t,.pts=pts}; return;
    }
}
static void add_splat(int x, int y, InvT t) {
    if (g.n_splat<MAX_SPLATS) g.splat[g.n_splat++]=(Splat){x,y,t};
}
static void check_soil(void) {
    if (g.n_splat>=MAX_SOIL && g.state==ST_PLAY) { g.state=ST_LOSE; commit_hiscore(); play_sfx(SFX_LOSE,1); }
}
static void schedule_ufo(void) {
    g.ufo.active=0; g.ufo.timer = 12.f + (float)(rand()%12000)/1000.f;
}

/* new grid + fresh barricades; keeps score, lives, wave, splats */
static void setup_wave(void) {
    float f = powf(0.9f,(float)(g.wave-1));
    g.base_step_ms = (Uint32)((float)DIFFS[g.difficulty].base_ms * f);
    if (g.base_step_ms < WAVE_MIN_MS) g.base_step_ms = WAVE_MIN_MS;
    g.step_ms = g.base_step_ms;
    g.max_bullets = DIFFS[g.difficulty].max_bullets;
    g.reload_ms   = DIFFS[g.difficulty].reload_ms;

    for (int r=0;r<GROWS;r++) for (int c=0;c<GCOLS;c++)
        g.inv[r][c]=(Invader){.x=(float)(GX+c*CW+CW/2),.y=(float)(GY+r*CH+CH/2),.alive=1,.type=(r<2)?T_MARTINI:T_BURGER};
    g.alive_count=GROWS*GCOLS; g.gox=g.goy=0.f; g.gdir=1; g.anim=0;
    g.last_step=SDL_GetTicks();

    static const int BXS[N_BARS]={170,360,540,730};
    for (int i=0;i<N_BARS;i++) g.bar[i]=(Bar){.x=BXS[i],.y=CARPET_Y-80,.hp=BAR_HP_MAX,.nst=0};

    memset(g.proj,0,sizeof g.proj); memset(g.ex,0,sizeof g.ex);
    schedule_ufo();
    g.px=(float)(SW/2); g.py=(float)(CARPET_Y-16);
    g.pdead=0; g.mopping=0; g.mop_t=0.f; g.last_shot=0;
    g.banner_t=1.6f; g.paused=0;
    g.state=ST_PLAY;
}

static void init_game(void) {
    g.score=0; g.lives=INIT_LIVES; g.wave=1; g.n_splat=0; g.new_hi=0;
    setup_wave();
}

static void wave_cleared(void) {
    g.state=ST_WAVE; g.wave_t=2.6f;
    g.wave_life = (g.lives<MAX_LIVES);
    if (g.wave_life) g.lives++;
    commit_hiscore();
    play_sfx(SFX_WIN,1);
}
static void next_wave(void) { g.wave++; setup_wave(); play_sfx(SFX_START,1); }

static void lose_game(void) {
    g.state=ST_LOSE; commit_hiscore(); play_sfx(SFX_LOSE,1);
}

/* ====================================================================
   INPUT
   ==================================================================== */
static void toggle_fullscreen(void) {
    Uint32 fl = SDL_GetWindowFlags(g.win) & SDL_WINDOW_FULLSCREEN_DESKTOP;
    SDL_SetWindowFullscreen(g.win, fl ? 0 : SDL_WINDOW_FULLSCREEN_DESKTOP);
}

static void fire(void) {
    if (g.pdead || g.mopping) return;
    int cnt=0;
    for (int i=0;i<MAX_PROJ;i++) if (g.proj[i].active&&g.proj[i].is_bullet) cnt++;
    Uint32 now=SDL_GetTicks();
    if (cnt>=g.max_bullets || (g.reload_ms && now-g.last_shot<g.reload_ms)) return;
    for (int i=0;i<MAX_PROJ;i++) if (!g.proj[i].active) {
        g.proj[i]=(Proj){.active=1,.is_bullet=1,.x=g.px,.y=g.py-20.f,.vy=-BULLET_SPD};
        g.last_shot=now; play_sfx(SFX_SHOOT,0); break;
    }
}

/* keyboard and gamepad both funnel into here */
static void handle_key(SDL_Keycode k) {
    if (k==SDLK_m){muted=!muted;return;}
    if (k==SDLK_f){toggle_fullscreen();return;}
    if (k==SDLK_ESCAPE) {
        if (g.state==ST_SELECT) g.running=0;
        else { if (g.state==ST_PLAY) commit_hiscore(); g.state=ST_SELECT; g.paused=0; }
        return;
    }
    if (g.state==ST_SELECT) {
        if (k==SDLK_UP  ||k==SDLK_w){g.sel_diff=(g.sel_diff+N_DIFF-1)%N_DIFF;return;}
        if (k==SDLK_DOWN||k==SDLK_s){g.sel_diff=(g.sel_diff+1)%N_DIFF;return;}
        if (k>=SDLK_1&&k<=SDLK_4) g.sel_diff=(int)(k-SDLK_1);
        if (k==SDLK_RETURN||k==SDLK_KP_ENTER||k==SDLK_SPACE||(k>=SDLK_1&&k<=SDLK_4)) {
            g.difficulty=g.sel_diff; init_game(); play_sfx(SFX_START,1);
        }
        return;
    }
    if (g.state==ST_WAVE) {
        if (k==SDLK_RETURN||k==SDLK_KP_ENTER||k==SDLK_SPACE) next_wave();
        return;
    }
    if (g.state==ST_LOSE) {
        if (k==SDLK_r||k==SDLK_RETURN||k==SDLK_KP_ENTER||k==SDLK_SPACE) g.state=ST_SELECT;
        return;
    }
    /* ST_PLAY */
    if (k==SDLK_p) {
        g.paused=!g.paused;
        if (!g.paused){Uint32 now=SDL_GetTicks(); g.last_step=now; g.last_shot=now-g.reload_ms;}
        return;
    }
    if (g.paused) return;
    if (k==SDLK_SPACE) fire();
}

static void open_pad(void) {
    if (g.pad) return;
    for (int i=0;i<SDL_NumJoysticks();i++)
        if (SDL_IsGameController(i) && (g.pad=SDL_GameControllerOpen(i))) return;
}

static void handle_event(const SDL_Event *e) {
    switch (e->type) {
    case SDL_QUIT: g.running=0; break;
    case SDL_KEYDOWN:
        /* ignore OS key-repeat except for Space, so holding it autofires */
        if (!e->key.repeat || e->key.keysym.sym==SDLK_SPACE) handle_key(e->key.keysym.sym);
        break;
    case SDL_CONTROLLERDEVICEADDED:   open_pad(); break;
    case SDL_CONTROLLERDEVICEREMOVED:
        if (g.pad && SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(g.pad))==e->cdevice.which)
            { SDL_GameControllerClose(g.pad); g.pad=NULL; open_pad(); }
        break;
    case SDL_CONTROLLERBUTTONDOWN:
        switch (e->cbutton.button) {
        case SDL_CONTROLLER_BUTTON_A:          handle_key(SDLK_SPACE);  break;
        case SDL_CONTROLLER_BUTTON_B:          handle_key(SDLK_ESCAPE); break;
        case SDL_CONTROLLER_BUTTON_X:          handle_key(SDLK_p);      break;
        case SDL_CONTROLLER_BUTTON_Y:          handle_key(SDLK_m);      break;
        case SDL_CONTROLLER_BUTTON_START:      handle_key(SDLK_RETURN); break;
        case SDL_CONTROLLER_BUTTON_BACK:       handle_key(SDLK_f);      break;
        case SDL_CONTROLLER_BUTTON_DPAD_UP:    if (g.state==ST_SELECT) handle_key(SDLK_UP);   break;
        case SDL_CONTROLLER_BUTTON_DPAD_DOWN:  if (g.state==ST_SELECT) handle_key(SDLK_DOWN); break;
        default: break;
        }
        break;
    default: break;
    }
}

/* ====================================================================
   UPDATE
   ==================================================================== */
static void hit_bar(Bar *b, float x, InvT t) {
    b->hp--;
    if (b->nst<BAR_STAINS) {
        int dx=(int)(x-(float)b->x); if (dx<-30) dx=-30; if (dx>30) dx=30;
        b->st[b->nst++]=(Stain){dx, rand()%12-6, t};
    }
}

static void update(float dt) {
    /* timers that run in every state */
    for (int i=0;i<MAX_EX;i++) if (g.ex[i].active && (g.ex[i].t+=dt)>=0.4f) g.ex[i].active=0;
    if (g.banner_t>0.f) g.banner_t-=dt;

    if (g.state==ST_WAVE) { if ((g.wave_t-=dt)<=0.f) next_wave(); return; }
    if (g.state!=ST_PLAY || g.paused) return;

    if (g.pdead>0) {
        if (--g.pdead==0) { if (g.lives<=0){lose_game();return;} g.px=(float)(SW/2); }
        return;
    }

    /* ── input: keyboard + gamepad ── */
    const uint8_t *ks=SDL_GetKeyboardState(NULL);
    int left =ks[SDL_SCANCODE_LEFT] ||ks[SDL_SCANCODE_A];
    int right=ks[SDL_SCANCODE_RIGHT]||ks[SDL_SCANCODE_D];
    int down =ks[SDL_SCANCODE_DOWN] ||ks[SDL_SCANCODE_S];
    if (g.pad) {
        Sint16 ax=SDL_GameControllerGetAxis(g.pad,SDL_CONTROLLER_AXIS_LEFTX);
        Sint16 ay=SDL_GameControllerGetAxis(g.pad,SDL_CONTROLLER_AXIS_LEFTY);
        left |= ax<-12000 || SDL_GameControllerGetButton(g.pad,SDL_CONTROLLER_BUTTON_DPAD_LEFT);
        right|= ax> 12000 || SDL_GameControllerGetButton(g.pad,SDL_CONTROLLER_BUTTON_DPAD_RIGHT);
        down |= ay> 12000 || SDL_GameControllerGetButton(g.pad,SDL_CONTROLLER_BUTTON_DPAD_DOWN);
    }

    float spd=PLAYER_SPD*dt;
    if (left)  g.px-=spd;
    if (right) g.px+=spd;
    if (g.px<25.f) g.px=25.f;
    if (g.px>(float)(SW-25)) g.px=(float)(SW-25);

    /* ── mopping: stand still, hold down, over a splat ── */
    g.mopping = down && !left && !right;
    if (g.mopping) {
        int best=-1; float bd=MOP_REACH;
        for (int i=0;i<g.n_splat;i++){ float d=fabsf((float)g.splat[i].x-g.px); if (d<bd){bd=d;best=i;} }
        if (best>=0) {
            g.mop_t+=dt;
            if (g.mop_t>=MOP_TIME) {
                g.splat[best]=g.splat[--g.n_splat];
                g.score+=5; g.mop_t=0.f; play_sfx(SFX_MOP,0);
            }
        } else g.mop_t=0.f;
    } else g.mop_t=0.f;

    /* ── invader grid step ── */
    Uint32 now=SDL_GetTicks();
    if (now-g.last_step>=g.step_ms) {
        g.last_step=now; g.anim^=1;
        float lx=99999.f,rx=-99999.f;
        for (int r=0;r<GROWS;r++) for (int c=0;c<GCOLS;c++) {
            if (!g.inv[r][c].alive) continue;
            float ax=g.inv[r][c].x+g.gox;
            if (ax<lx) lx=ax;
            if (ax>rx) rx=ax;
        }
        float step=(float)g.gdir*CW*0.55f; int bounce=0;
        if (g.gdir== 1&&rx+step>SW-28.f){g.gdir=-1;bounce=1;}
        if (g.gdir==-1&&lx+step<28.f)   {g.gdir= 1;bounce=1;}
        if (bounce) {
            g.goy+=20.f;
            for (int r=0;r<GROWS;r++) for (int c=0;c<GCOLS;c++)
                if (g.inv[r][c].alive&&g.inv[r][c].y+g.goy>=CARPET_Y){lose_game();return;}
        } else g.gox+=step;

        int active_drops=0;
        for (int i=0;i<MAX_PROJ;i++) if (g.proj[i].active&&!g.proj[i].is_bullet) active_drops++;
        int max_drops=1+(GROWS*GCOLS-g.alive_count)/7+(g.wave-1);
        if (max_drops>6) max_drops=6;
        if (active_drops<max_drops) {
            int col=rand()%GCOLS, cr=-1;
            for (int r=GROWS-1;r>=0;r--) if (g.inv[r][col].alive){cr=r;break;}
            if (cr>=0) for (int i=0;i<MAX_PROJ;i++) if (!g.proj[i].active) {
                g.proj[i]=(Proj){.active=1,.is_bullet=0,.x=g.inv[cr][col].x+g.gox,
                                 .y=g.inv[cr][col].y+g.goy+16.f,.vy=DROP_SPD,.dtype=g.inv[cr][col].type};
                break;
            }
        }
    }

    /* ── Bloody Mary flyer ── */
    if (!g.ufo.active) {
        if ((g.ufo.timer-=dt)<=0.f) {
            int dir = rand()%2 ? 1 : -1;
            g.ufo=(Ufo){.active=1,.dropped=0,.worth=50*(1+rand()%3),
                        .x=dir>0?-30.f:(float)SW+30.f,.vx=(float)dir*UFO_SPD,
                        .drop_x=150.f+(float)(rand()%600),.timer=0.f};
        }
    } else {
        g.ufo.x+=g.ufo.vx*dt;
        if (!g.ufo.dropped && ((g.ufo.vx>0&&g.ufo.x>=g.ufo.drop_x)||(g.ufo.vx<0&&g.ufo.x<=g.ufo.drop_x))) {
            g.ufo.dropped=1;
            for (int i=0;i<MAX_PROJ;i++) if (!g.proj[i].active) {
                g.proj[i]=(Proj){.active=1,.is_bullet=0,.x=g.ufo.x,.y=(float)UFO_Y+14.f,.vy=DROP_SPD*1.25f,.dtype=T_BLOODY};
                break;
            }
        }
        if (g.ufo.x<-40.f||g.ufo.x>(float)SW+40.f) schedule_ufo();
    }

    /* ── projectiles ── */
    for (int i=0;i<MAX_PROJ;i++) {
        Proj *p=&g.proj[i];
        if (!p->active) continue;
        p->y+=p->vy*dt;

        if (p->is_bullet) {
            if (p->y<HUD_H){p->active=0;continue;}
            /* vs flyer */
            if (g.ufo.active && fabsf(p->x-g.ufo.x)<16.f && fabsf(p->y-(float)UFO_Y)<14.f) {
                p->active=0; g.score+=g.ufo.worth;
                add_explosion(g.ufo.x,(float)UFO_Y,T_BLOODY,g.ufo.worth);
                play_sfx(SFX_BONUS,1); schedule_ufo(); continue;
            }
            int hit=0;
            for (int r=0;r<GROWS&&!hit;r++) for (int c=0;c<GCOLS&&!hit;c++) {
                Invader *v=&g.inv[r][c];
                if (!v->alive) continue;
                float ax=v->x+g.gox, ay=v->y+g.goy;
                if (fabsf(p->x-ax)<16.f&&fabsf(p->y-ay)<14.f) {
                    v->alive=0; g.alive_count--; p->active=0; hit=1;
                    int pts=(v->type==T_MARTINI)?20:10; g.score+=pts;
                    add_explosion(ax,ay,v->type,pts);
                    float frac=1.f-(float)g.alive_count/(GROWS*GCOLS);
                    g.step_ms=g.base_step_ms-(Uint32)((float)(g.base_step_ms-MIN_MS)*frac);
                    if (g.step_ms<MIN_MS) g.step_ms=MIN_MS;
                    if (g.alive_count==0){wave_cleared();return;}
                }
            }
            if (!hit) for (int b=0;b<N_BARS;b++) {
                if (g.bar[b].hp<=0) continue;
                if (fabsf(p->x-g.bar[b].x)<38.f&&fabsf(p->y-g.bar[b].y)<14.f){g.bar[b].hp--;p->active=0;break;}
            }
        } else {
            if (p->y>SH){p->active=0;continue;}
            int hit=0;
            for (int b=0;b<N_BARS&&!hit;b++) {
                if (g.bar[b].hp<=0) continue;
                if (fabsf(p->x-g.bar[b].x)<38.f&&fabsf(p->y-g.bar[b].y)<16.f){hit_bar(&g.bar[b],p->x,p->dtype);p->active=0;hit=1;}
            }
            if (!hit && fabsf(p->x-g.px)<12.f&&fabsf(p->y-g.py)<18.f) {
                InvT t=p->dtype; p->active=0; hit=1;
                g.lives--; g.pdead=90; g.mopping=0; g.mop_t=0.f;
                play_sfx(SFX_LIFE,1);
                add_splat((int)g.px+(rand()%24-12),CARPET_Y+15+rand()%20,t);
                if (t==T_BLOODY) add_splat((int)g.px+(rand()%24-12),CARPET_Y+15+rand()%20,t);
                check_soil(); if (g.state!=ST_PLAY) return;
            }
            if (!hit&&p->y>=CARPET_Y) {
                InvT t=p->dtype; p->active=0;
                int sx=(int)p->x;
                if (t==T_BLOODY) {
                    play_sfx(SFX_BLOODY,1);
                    add_splat(sx-7+rand()%5,CARPET_Y+8+rand()%25,t);
                    add_splat(sx+7+rand()%5,CARPET_Y+8+rand()%25,t);
                } else {
                    play_sfx(t==T_BURGER?SFX_FAT:SFX_DRIP,0);
                    add_splat(sx+(rand()%8-4),CARPET_Y+8+rand()%25,t);
                }
                check_soil(); if (g.state!=ST_PLAY) return;
            }
        }
    }
}

/* ====================================================================
   RENDER – SELECT SCREEN
   ==================================================================== */
static void render_select(void) {
    SDL_Renderer *ren=g.ren;
    RGB(0x08,0x06,0x14); SDL_RenderClear(ren);
    RGB(0x0C,0x09,0x1C); for (int x=40;x<SW;x+=60) SDL_RenderDrawLine(ren,x,0,x,SH);

    RGB(0x80,0x60,0xC8); draw_cx(ren,SW/2,42,"THE HOUSEKEEPER",3);
    RGB(0x60,0x60,0x90); draw_cx(ren,SW/2,90,"SELECT DIFFICULTY",2);

    g.mopping=0; g.pdead=0;
    draw_housekeeper(ren,90,220);
    draw_burger(ren,SW-78,200,0); draw_martini(ren,SW-78,258,0); draw_bloody(ren,SW-78,320);

    int bx=170,by0=128,bw=560,bh=68,gap=10;
    for (int d=0;d<N_DIFF;d++) {
        int by=by0+d*(bh+gap), sel=(d==g.sel_diff);
        SDL_Color c=DIFF_COL[d];
        if (sel) RGBA(c.r/5,c.g/5,c.b/5,255); else RGB(0x10,0x0E,0x20);
        SDL_Rect bg={bx,by,bw,bh}; SDL_RenderFillRect(ren,&bg);
        if (sel) RGBA(c.r,c.g,c.b,255); else RGB(0x28,0x22,0x44);
        SDL_RenderDrawRect(ren,&bg);
        RGB(0x70,0x70,0x90); char kb[2]={(char)('1'+d),0}; draw_str(ren,bx+10,by+10,kb,2);
        RGBA(c.r,c.g,c.b,255); draw_str(ren,bx+36,by+10,DIFFS[d].name,2);
        RGB(0x90,0x88,0xA8);
        draw_str(ren,bx+36,by+34,DIFFS[d].desc[0],1); draw_str(ren,bx+36,by+46,DIFFS[d].desc[1],1);
        if (g.hi[d]>0) { RGB(0xE0,0xD0,0x60); draw_strf(ren,bx+bw-160,by+12,1,"BEST %d",g.hi[d]); }
        if (sel) { RGBA(c.r,c.g,c.b,255); int ax=bx+bw-16,ay=by+bh/2;
                   SDL_RenderDrawLine(ren,ax-8,ay-7,ax,ay); SDL_RenderDrawLine(ren,ax-8,ay+7,ax,ay); }
    }
    RGB(0x48,0x48,0x68);
    draw_cx(ren,SW/2,by0+N_DIFF*(bh+gap)+16,"UP DOWN OR 1 2 3 4  THEN ENTER",1);
    draw_cx(ren,SW/2,by0+N_DIFF*(bh+gap)+32,"HOLD DOWN OVER A STAIN TO MOP IT UP",1);
    if (g.pad) { RGB(0x60,0x90,0x60); draw_cx(ren,SW/2,SH-24,"GAMEPAD CONNECTED",1); }
    SDL_RenderPresent(ren);
}

/* ====================================================================
   RENDER – GAME
   ==================================================================== */
static void render_game(void) {
    SDL_Renderer *ren=g.ren;
    RGB(0x0C,0x0A,0x18); SDL_RenderClear(ren);
    RGB(0x0F,0x0C,0x1E); for (int x=30;x<SW;x+=60) SDL_RenderDrawLine(ren,x,HUD_H,x,CARPET_Y);

    RGB(0x38,0x20,0x0C); SDL_Rect carpet={0,CARPET_Y,SW,SH-CARPET_Y}; SDL_RenderFillRect(ren,&carpet);
    RGB(0x2A,0x16,0x08);
    for (int x=0;x<SW;x+=14) SDL_RenderDrawLine(ren,x,CARPET_Y,x,SH);
    for (int y=CARPET_Y;y<SH;y+=10) SDL_RenderDrawLine(ren,0,y,SW,y);

    for (int i=0;i<g.n_splat;i++) draw_splat(ren,g.splat[i].x,g.splat[i].y,g.splat[i].type);
    for (int i=0;i<N_BARS;i++)    draw_barricade(ren,&g.bar[i]);

    for (int r=0;r<GROWS;r++) for (int c=0;c<GCOLS;c++) {
        if (!g.inv[r][c].alive) continue;
        int ax=(int)(g.inv[r][c].x+g.gox),ay=(int)(g.inv[r][c].y+g.goy);
        if (g.inv[r][c].type==T_BURGER) draw_burger(ren,ax,ay,g.anim); else draw_martini(ren,ax,ay,g.anim);
    }
    if (g.ufo.active) draw_bloody(ren,(int)g.ufo.x,UFO_Y);

    for (int i=0;i<MAX_PROJ;i++) {
        if (!g.proj[i].active) continue;
        int px2=(int)g.proj[i].x,py2=(int)g.proj[i].y;
        if (g.proj[i].is_bullet) draw_bullet(ren,px2,py2); else draw_drop(ren,px2,py2,g.proj[i].dtype);
    }
    draw_housekeeper(ren,(int)g.px,(int)g.py);
    for (int i=0;i<MAX_EX;i++) if (g.ex[i].active) draw_explosion(ren,&g.ex[i]);

    /* ── HUD ── */
    RGB(0x06,0x06,0x10); SDL_Rect hud={0,0,SW,HUD_H}; SDL_RenderFillRect(ren,&hud);
    RGB(0x30,0x30,0x48); SDL_RenderDrawLine(ren,0,HUD_H,SW,HUD_H);
    RGB(0xE0,0xE0,0xE0); draw_strf(ren,35,10,2,"SCORE %d",g.score);
    RGB(0xE0,0xD0,0x60); draw_strf(ren,215,10,2,"HI %d",g.hi[g.difficulty]>g.score?g.hi[g.difficulty]:g.score);
    RGB(0xA0,0xA0,0xC0); draw_strf(ren,380,10,2,"WAVE %d",g.wave);
    for (int i=0;i<g.lives&&i<MAX_LIVES;i++) {
        int ix=500+i*24;
        RGB(0xD4,0x7C,0x1E); SDL_Rect bn={ix,13,18,9}; SDL_RenderFillRect(ren,&bn);
        RGB(0x52,0x26,0x06); SDL_Rect pt={ix+1,19,16,5}; SDL_RenderFillRect(ren,&pt);
    }
    SDL_Color dc=DIFF_COL[g.difficulty]; RGBA(dc.r,dc.g,dc.b,255);
    draw_str(ren,640,10,DIFFS[g.difficulty].name,2);
    if (muted) { RGB(0x90,0x40,0x40); draw_str(ren,816,10,"MUTE",2); }

    if (g.reload_ms>0 && !g.pdead && g.state==ST_PLAY) {
        Uint32 el=SDL_GetTicks()-g.last_shot;
        if (el<g.reload_ms) {
            int fw=120, filled=(int)((float)el/(float)g.reload_ms*(float)fw);
            int bx2=(int)g.px-fw/2, by2=(int)g.py+20;
            RGB(0x28,0x28,0x28); SDL_Rect bg2={bx2,by2,fw,4}; SDL_RenderFillRect(ren,&bg2);
            RGB(0xFF,0xE0,0x40); SDL_Rect fg={bx2,by2,filled,4}; SDL_RenderFillRect(ren,&fg);
        }
    }
    draw_soil_meter(ren);

    /* wave banner */
    if (g.banner_t>0.f && g.state==ST_PLAY) {
        SDL_SetRenderDrawBlendMode(ren,SDL_BLENDMODE_BLEND);
        Uint8 a=(Uint8)(g.banner_t>0.5f?255:g.banner_t*510.f);
        RGBA(0xE0,0xE0,0x60,a); draw_cxf(ren,SW/2,300,4,"WAVE %d",g.wave);
        SDL_SetRenderDrawBlendMode(ren,SDL_BLENDMODE_NONE);
    }

    /* overlays */
    if (g.paused && g.state==ST_PLAY) {
        SDL_SetRenderDrawBlendMode(ren,SDL_BLENDMODE_BLEND); RGBA(0,0,0,160);
        SDL_Rect ol={0,HUD_H,SW,SH-HUD_H}; SDL_RenderFillRect(ren,&ol);
        SDL_SetRenderDrawBlendMode(ren,SDL_BLENDMODE_NONE);
        RGB(0xE0,0xE0,0x60); draw_cx(ren,SW/2,270,"PAUSED",4);
        RGB(0x90,0x90,0x90); draw_cx(ren,SW/2,330,"P TO RESUME  ESC TO ABANDON",2);
    }
    if (g.state==ST_WAVE) {
        SDL_SetRenderDrawBlendMode(ren,SDL_BLENDMODE_BLEND); RGBA(0,0,0,175);
        SDL_Rect ol={60,180,SW-120,230}; SDL_RenderFillRect(ren,&ol);
        SDL_SetRenderDrawBlendMode(ren,SDL_BLENDMODE_NONE);
        RGB(0x60,0xFF,0x80); draw_cxf(ren,SW/2,210,4,"WAVE %d CLEARED",g.wave);
        RGB(0xC0,0xC0,0xC0); draw_cx(ren,SW/2,280,"THE FURNITURE IS TIDIED. THE CARPET IS NOT.",2);
        RGB(0xE0,0xE0,0x60); draw_cx(ren,SW/2,318,g.wave_life?"+1 LIFE":"LIVES FULL",2);
        RGB(0xD0,0xD0,0x40); draw_cxf(ren,SW/2,370,2,"NEXT WAVE IN %d   ENTER TO SKIP",(int)g.wave_t+1);
    }
    if (g.state==ST_LOSE) {
        SDL_SetRenderDrawBlendMode(ren,SDL_BLENDMODE_BLEND); RGBA(0,0,0,175);
        SDL_Rect ol={60,180,SW-120,230}; SDL_RenderFillRect(ren,&ol);
        SDL_SetRenderDrawBlendMode(ren,SDL_BLENDMODE_NONE);
        RGB(0xFF,0x40,0x40); draw_cx(ren,SW/2,210,"GAME OVER",4);
        RGB(0xC0,0xC0,0xC0); draw_cx(ren,SW/2,280,"THE CARPET IS RUINED!",2);
        RGB(0xE0,0xE0,0x60); draw_cxf(ren,SW/2,318,2,"SCORE %d   WAVE %d",g.score,g.wave);
        if (g.new_hi) { RGB(0xFF,0xFF,0xA0); draw_cx(ren,SW/2,344,"NEW HIGH SCORE!",2); }
        RGB(0xD0,0xD0,0x40); draw_cx(ren,SW/2,376,"R OR ENTER: BACK TO SELECT",2);
    }
    SDL_RenderPresent(ren);
}

/* ====================================================================
   MAIN LOOP
   ==================================================================== */
static void main_loop(void) {
    Uint32 now=SDL_GetTicks();
    float dt=(float)(now-g.prev)/1000.f; if (dt>0.05f) dt=0.05f;
    g.prev=now;
    SDL_Event e; while (SDL_PollEvent(&e)) handle_event(&e);
    if (g.state==ST_SELECT) render_select();
    else { update(dt); render_game(); }
}

/* ====================================================================
   MAIN
   ==================================================================== */
static void usage(FILE *f) {
    fputs(
"Usage: housekeeper [OPTIONS]\n\n"
"  -d, --difficulty N   start at difficulty N (1=easy .. 4=nightmare)\n"
"  -f, --fullscreen     start in fullscreen\n"
"  -m, --mute           start muted (M toggles)\n"
"  -h, --help           show this help and exit\n"
"  -V, --version        show version and exit\n", f);
}

int main(int argc, char *argv[]) {
    int start_diff=-1, want_fs=0;
    for (int i=1;i<argc;i++) {
        const char *a=argv[i];
        if (!strcmp(a,"-h")||!strcmp(a,"--help"))       { usage(stdout); return 0; }
        if (!strcmp(a,"-V")||!strcmp(a,"--version"))    { puts("housekeeper " HK_VERSION); return 0; }
        if (!strcmp(a,"-m")||!strcmp(a,"--mute"))       { muted=1; continue; }
        if (!strcmp(a,"-f")||!strcmp(a,"--fullscreen")) { want_fs=1; continue; }
        if (!strcmp(a,"-d")||!strcmp(a,"--difficulty")) {
            if (i+1>=argc){fprintf(stderr,"%s: missing argument\n",a);usage(stderr);return 2;}
            int n=atoi(argv[++i]);
            if (n<1||n>N_DIFF){fprintf(stderr,"difficulty must be 1..%d\n",N_DIFF);return 2;}
            start_diff=n-1; continue;
        }
        fprintf(stderr,"unknown option: %s\n",a); usage(stderr); return 2;
    }

    if (SDL_Init(SDL_INIT_VIDEO|SDL_INIT_AUDIO|SDL_INIT_GAMECONTROLLER)<0) {
        fprintf(stderr,"SDL_Init: %s\n",SDL_GetError()); return 1;
    }
    g.win=SDL_CreateWindow("The Housekeeper",SDL_WINDOWPOS_CENTERED,SDL_WINDOWPOS_CENTERED,SW,SH,
        SDL_WINDOW_RESIZABLE|(want_fs?SDL_WINDOW_FULLSCREEN_DESKTOP:0));
    if (!g.win){fprintf(stderr,"SDL_CreateWindow: %s\n",SDL_GetError());SDL_Quit();return 1;}
    g.ren=SDL_CreateRenderer(g.win,-1,SDL_RENDERER_ACCELERATED|SDL_RENDERER_PRESENTVSYNC);
    if (!g.ren) g.ren=SDL_CreateRenderer(g.win,-1,SDL_RENDERER_SOFTWARE);
    if (!g.ren){fprintf(stderr,"SDL_CreateRenderer: %s\n",SDL_GetError());SDL_DestroyWindow(g.win);SDL_Quit();return 1;}
    SDL_RenderSetLogicalSize(g.ren,SW,SH);
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY,"0");

    srand((unsigned int)time(NULL)^(unsigned int)SDL_GetTicks());
    init_audio(); atexit(free_audio);
    hi_load(); open_pad();

    g.state=ST_SELECT; g.sel_diff=0; g.running=1; g.prev=SDL_GetTicks();
    if (start_diff>=0) { g.sel_diff=g.difficulty=start_diff; init_game(); play_sfx(SFX_START,1); }

#ifdef __EMSCRIPTEN__
    emscripten_set_main_loop(main_loop,0,1);
#else
    while (g.running) {
        Uint32 t0=SDL_GetTicks(); main_loop();
        Uint32 el=SDL_GetTicks()-t0; if (el<16) SDL_Delay(16-el);
    }
    if (g.pad) SDL_GameControllerClose(g.pad);
    SDL_DestroyRenderer(g.ren); SDL_DestroyWindow(g.win); SDL_Quit();
#endif
    return 0;
}
