// SPDX-License-Identifier: MIT
// Copyright (c) 2026 rudibr
// game.c - BYTE RUN
// An original maze-chase game for the SwissMicros DM42 (DMCP SDK).
// All artwork, maze layout, names and sounds are original to this program.
//
// Portable C: talks to the hardware only through plat.h.

#include <stdint.h>
#include <string.h>
#include "plat.h"
#include "game.h"

// ---------------------------------------------------------------------------
//  Geometry
// ---------------------------------------------------------------------------
#define MW    31
#define MH    23
#define TILE  10
#define MX0   0            // maze origin on screen
#define MY0   5
#define MAZE_PX_W (MW*TILE)
#define PANEL_X   (MAZE_PX_W + 4)
#define PANEL_W   (SCR_W - PANEL_X)

#define SUB   16           // sub-pixel units per pixel
#define TS    (TILE*SUB)   // tile size in sub units (160)
#define HALF  (TS/2)

#define SEC(s) ((int)((s) * (1000 / TICK_MS)))

#define DOOR_X 15
#define DOOR_Y 10
#define OUT_Y   9          // tile just above the door
#define PEN_Y  11
#define HERO_X 15
#define HERO_Y 17

static const char maze_src[MH][MW + 1] = {
  "###############################",
  "#o............###............o#",
  "#.###.###.###.###.###.###.###.#",
  "#.....###.....###.....###.....#",
  "#.###.###.###########.###.###.#",
  "#.............................#",
  "#.###.###.##.#####.##.###.###.#",
  "#.....###....#####....###.....#",
  "#####.###################.#####",
  "#####.                   .#####",
  "#####.### #####-##### ###.#####",
  "     .### ##       ## ###.     ",
  "#####.### ########### ###.#####",
  "#####.                   .#####",
  "#####.###################.#####",
  "#.....###.............###.....#",
  "#.###.###.###########.###.###.#",
  "#.............. ..............#",
  "#.###.#.#####.###.#####.#.###.#",
  "#...#.#.......###.......#.#...#",
  "#.#.#.#######.###.#######.#.#.#",
  "#o...........................o#",
  "###############################",
};

static char maze[MH][MW];

// Directions: 0 up, 1 left, 2 down, 3 right
static const int DX[4] = { 0, -1, 0, 1 };
static const int DY[4] = { -1, 0, 1, 0 };
#define OPP(d) (((d) + 2) & 3)

// ---------------------------------------------------------------------------
//  State
// ---------------------------------------------------------------------------
enum { GS_TITLE, GS_READY, GS_PLAY, GS_DYING, GS_CLEAR, GS_OVER, GS_PAUSE };
enum { EM_PEN, EM_LEAVE, EM_ACTIVE, EM_EYES, EM_ENTER };

typedef struct { int x, y, dir, ndir; } mover_t;

typedef struct {
  mover_t m;
  int mode;
  int fright;
  int id;
  int home_x, home_y;  // sub units
} enemy_t;

static int state, prev_state, timer;
static uint32_t ticks;
static mover_t hero;
static int hero_anim;
static enemy_t en[4];

static uint32_t score, high, next_life;
static int lives, level;
static int dots_left, dots_eaten_level, dots_eaten_life;
static int life_ticks;            // ticks since play started this life
static int mode_phase, mode_timer, scatter;
static int fright_timer, chain;
static int freeze;                // short freeze after eating an enemy
static int popup_val, popup_x, popup_y, popup_t;
static int bonus_t, bonus_shown;  // bonus chip
static int walls_hidden, power_vis;
static int sound_on, sound_mode;   // mode: 0 off, 1 effects only, 2 all
static int bg_hold;
static const char *const SND_NAME[3] = { "OFF", "FX", "ALL" };
static int cfg_dirty;
static uint32_t rng = 0x1234567u;

// ---------------------------------------------------------------------------
//  Small helpers
// ---------------------------------------------------------------------------
static int pmod(int a, int m) { a %= m; return a < 0 ? a + m : a; }

static uint32_t rnd(void) {
  rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5;
  return rng;
}

static void utoa10(uint32_t v, char *s) {
  char t[12]; int n = 0;
  do { t[n++] = '0' + v % 10; v /= 10; } while (v);
  while (n) *s++ = t[--n];
  *s = 0;
}

static char tile_at(int tx, int ty) {
  if (ty < 0 || ty >= MH) return '#';
  return maze[ty][pmod(tx, MW)];
}

static int passable(int tx, int ty) {
  char c = tile_at(tx, ty);
  return c != '#' && c != '-';
}

// Clipped drawing into the maze area / screen
static void rect_clip(int x, int y, int w, int h, int black, int x0, int x1) {
  if (x < x0) { w -= x0 - x; x = x0; }
  if (x + w > x1) w = x1 - x;
  if (y < 0) { h += y; y = 0; }
  if (y + h > SCR_H) h = SCR_H - y;
  if (w > 0 && h > 0) plat_rect(x, y, w, h, black);
}
#define MRECT(x, y, w, h, b) rect_clip(x, y, w, h, b, MX0, MX0 + MAZE_PX_W)

// Draw a bitmap given as strings: '#' black, 'o' white, other = transparent.
static void blit(int x, int y, const char *const *rows, int n, int clip) {
  for (int r = 0; r < n; r++) {
    const char *s = rows[r];
    for (int c = 0; s[c];) {
      char ch = s[c];
      if (ch != '#' && ch != 'o') { c++; continue; }
      int c0 = c;
      while (s[c] == ch) c++;
      if (clip) MRECT(x + c0, y + r, c - c0, 1, ch == '#');
      else      rect_clip(x + c0, y + r, c - c0, 1, ch == '#', 0, SCR_W);
    }
  }
}

// ---------------------------------------------------------------------------
//  Sound (non-blocking note queue)
// ---------------------------------------------------------------------------
// Each note can glide from hz to hz2 and can have vibrato, which makes the
// single piezo buzzer sound a lot livelier than plain beeps.
typedef struct { uint16_t hz, hz2, ms; uint8_t fx; } note_t;
#define N(h, ms)      { h, h, ms, 0 }
#define S(h, h2, ms)  { h, h2, ms, 0 }   // glide
#define V(h, ms)      { h, h, ms, 1 }    // vibrato
#define VS(h, h2, ms) { h, h2, ms, 1 }
#define R(ms)         { 0, 0, ms, 0 }    // rest

// note frequencies (Hz)
enum { C6 = 1047, D6 = 1175, E6 = 1319, F6 = 1397, G6 = 1568, A6 = 1760, B6 = 1976,
       C7 = 2093, D7 = 2349, E7 = 2637, G7 = 3136 };

static const note_t SND_DOT1[]  = { S(1500, 800, 32) };
static const note_t SND_DOT2[]  = { S(800, 1400, 32) };
static const note_t SND_POWER[] = { VS(500, 2600, 220), S(2600, 1800, 60) };
static const note_t SND_EAT[]   = { S(500, 3400, 170), R(20), N(3400, 30) };
static const note_t SND_BONUS[] = { S(2000, 3000, 50), S(2400, 3400, 50), S(2800, 3800, 80) };
static const note_t SND_LIFE[]  = { N(E7, 70), R(30), N(E7, 70), R(30), N(G7, 70), R(30), V(E7, 240) };
static const note_t SND_CLEAR[] = { N(G6, 70), N(C7, 70), N(E7, 70), N(G7, 70), R(30),
                                    N(E7, 70), V(G7, 300) };
// Start-up tune: an original 4-bar arpeggio piece
static const note_t SND_START[] = {
  N(C6,105),R(15), N(E6,105),R(15), N(G6,105),R(15), N(C7,105),R(15),
  N(B6,105),R(15), N(G6,105),R(15), V(D7,330),R(30),
  N(A6,105),R(15), N(F6,105),R(15), N(C7,105),R(15), N(A6,105),R(15),
  N(G6,105),R(15), N(E6,105),R(15), V(C7,330),R(30),
  N(D6,105),R(15), N(F6,105),R(15), N(A6,105),R(15), N(D7,105),R(15),
  N(E7,105),R(15), N(D7,105),R(15), N(C7,105),R(15), N(B6,105),R(15),
  VS(C7, C7, 480), S(C7, G7, 60), R(40), N(G7, 50), R(30), N(C7, 160)
};
static const note_t SND_DEATH[] = {
  VS(2400, 1700, 130), VS(2200, 1500, 130), VS(2000, 1300, 130), VS(1800, 1100, 130),
  VS(1600, 900, 130),  VS(1400, 700, 140),  VS(1200, 500, 160),  R(80),
  S(600, 300, 90), R(60), S(600, 300, 90) };
static const note_t SND_ON[]    = { S(1200, 2400, 60), R(20), N(2400, 50) };
// background loops (only with sound mode ALL)
static const note_t BG_FRIGHT[] = { S(700, 1150, 90), S(1150, 700, 90) };
static const note_t BG_EYES[]   = { S(1800, 3300, 45), R(10) };
static note_t BG_HUM[2];

#define NOTES(a) a, (int)(sizeof(a) / sizeof(a[0]))

static const note_t *sq; static int sq_n, sq_i, sq_prio, sq_hz;
static uint32_t sq_t0;

static void tone(int hz) { if (hz != sq_hz) { sq_hz = hz; plat_tone(hz); } }

static void sfx_stop(void) { sq = 0; sq_prio = 0; sq_hz = -1; plat_tone(0); }

static void sfx_start_note(void) {
  sq_t0 = plat_ms();
  tone(sq[sq_i].hz);
}

static void sfx(const note_t *n, int cnt, int prio) {
  if (!sound_on) return;
  if (sq && prio < sq_prio) return;
  if (prio > 0) bg_hold = 3;
  sq = n; sq_n = cnt; sq_i = 0; sq_prio = prio;
  sfx_start_note();
}

void game_sfx_update(void) {
  if (!sq) return;
  uint32_t now = plat_ms();
  while (sq && now - sq_t0 >= sq[sq_i].ms) {
    uint32_t end = sq_t0 + sq[sq_i].ms;
    if (++sq_i >= sq_n) { sfx_stop(); return; }
    sfx_start_note();
    sq_t0 = end;
  }
  const note_t *n = &sq[sq_i];
  if (!n->hz) { tone(0); return; }
  int el = (int)(now - sq_t0);
  int hz = n->hz + ((int)n->hz2 - (int)n->hz) * el / (n->ms ? n->ms : 1);
  if (n->fx & 1) {                       // ~14 Hz vibrato, +-3 %
    int ph = (el / 9) % 8;
    int tri = ph < 4 ? ph : 8 - ph;      // 0..4
    hz += hz * (tri - 2) * 3 / 200;
  }
  tone(hz / 8 * 8);                      // quantise a little: fewer buzzer updates
}

static int sfx_busy(void) { return sq != 0; }

// ---------------------------------------------------------------------------
//  Sprites (9x9, original designs)
// ---------------------------------------------------------------------------
// Hero "Byte": a little robot. Eyes are drawn separately.
static const char *const HERO_A[9] = {
  "....#....",
  "..#####..",
  ".#######.",
  "#########",
  "#########",
  "#########",
  ".#######.",
  ".##...##.",
  ".........",
};
static const char *const HERO_B[9] = {
  "....#....",
  "..#####..",
  ".#######.",
  "#########",
  "#########",
  "#########",
  ".#######.",
  "..##.##..",
  ".........",
};

// Enemy "Glitch": a spinning spiky bug, two frames.
static const char *const GL_A[9] = {
  "#.......#",
  ".#.###.#.",
  "..#####..",
  ".#######.",
  "#########",
  ".#######.",
  "..#####..",
  ".#.###.#.",
  "#.......#",
};
static const char *const GL_B[9] = {
  "....#....",
  "..#####..",
  ".#######.",
  ".#######.",
  "#########",
  ".#######.",
  ".#######.",
  "..#####..",
  "....#....",
};
// Frightened: hollow
static const char *const GL_F[9] = {
  "#.......#",
  ".#.###.#.",
  "..#ooo#..",
  ".#ooooo#.",
  "#ooooooo#",
  ".#o#o#o#.",
  "..#o#o#..",
  ".#.###.#.",
  "#.......#",
};
// Returning core
static const char *const GL_EYES[9] = {
  ".........",
  ".........",
  "....#....",
  "...#o#...",
  "..#ooo#..",
  "...#o#...",
  "....#....",
  ".........",
  ".........",
};
// Bonus chip
static const char *const CHIP[9] = {
  ".........",
  ".#.#.#.#.",
  "#########",
  "#o#######",
  "#########",
  "#########",
  "#########",
  ".#.#.#.#.",
  ".........",
};

// 3x5 digits for score popups
static const char *const DIG[10][5] = {
  {"###","#.#","#.#","#.#","###"}, {".#.","##.",".#.",".#.","###"},
  {"###","..#","###","#..","###"}, {"###","..#",".##","..#","###"},
  {"#.#","#.#","###","..#","..#"}, {"###","#..","###","..#","###"},
  {"###","#..","###","#.#","###"}, {"###","..#",".#.",".#.",".#."},
  {"###","#.#","###","#.#","###"}, {"###","#.#","###","..#","###"},
};

static void draw_hero_at(int x, int y, int dir, int frame) {
  blit(x, y, frame ? HERO_B : HERO_A, 9, 1);
  // eyes (white 2x2) shifted toward facing direction
  int ex = 2, ey = 3;
  if (dir == 1) ex = 1; else if (dir == 3) ex = 3;
  if (dir == 0) ey = 2; else if (dir == 2) ey = 4;
  MRECT(x + ex, y + ey, 2, 2, 0);
  MRECT(x + ex + 3, y + ey, 2, 2, 0);
}

// Hero dissolving (death animation), k = 0..8
static void draw_hero_dissolve(int x, int y, int k) {
  for (int r = 0; r < 9; r++)
    for (int c = 0; c < 9; c++) {
      if (HERO_A[r][c] != '#') continue;
      int h = (r * 7 + c * 5 + (r * c) * 3) % 9;
      if (h >= k) {
        int sx = (c - 4) * k / 4, sy = (r - 4) * k / 4;  // pixels scatter outward
        MRECT(x + c + sx, y + r + sy, 1, 1, 1);
      }
    }
}

static void draw_enemy(const enemy_t *e, int x, int y) {
  if (e->mode == EM_EYES || e->mode == EM_ENTER) { blit(x, y, GL_EYES, 9, 1); return; }
  int frame = (ticks / 4) & 1;
  if (e->fright) {
    int flash = fright_timer < SEC(2) && ((ticks / 5) & 1);
    blit(x, y, flash ? (frame ? GL_B : GL_A) : GL_F, 9, 1);
    return;
  }
  blit(x, y, frame ? GL_B : GL_A, 9, 1);
  // eyes: white 2x2 with a black pupil looking where it goes
  int d = e->m.dir;
  MRECT(x + 2, y + 3, 2, 2, 0);
  MRECT(x + 5, y + 3, 2, 2, 0);
  int px = (d == 1) ? 0 : 1, py = (d == 0) ? 0 : 1;
  MRECT(x + 2 + px, y + 3 + py, 1, 1, 1);
  MRECT(x + 5 + px, y + 3 + py, 1, 1, 1);
  // individual marks so the four can be told apart
  if (e->id == 1) MRECT(x + 3, y + 6, 3, 1, 0);
  if (e->id == 2) { MRECT(x + 3, y + 6, 1, 1, 0); MRECT(x + 5, y + 6, 1, 1, 0); }
  if (e->id == 3) MRECT(x + 4, y + 1, 1, 2, 0);
}

static int draw_num_tiny(int x, int y, uint32_t v, int measure) {
  char s[12]; utoa10(v, s);
  int w = (int)strlen(s) * 4 - 1;
  if (measure) return w;
  for (int i = 0; s[i]; i++) blit(x + i * 4, y, DIG[s[i] - '0'], 5, 1);
  return w;
}

// ---------------------------------------------------------------------------
//  Maze drawing
// ---------------------------------------------------------------------------
static int wopen(int tx, int ty) {  // open (non-wall) for outline purposes
  if (tx < 0 || tx >= MW || ty < 0 || ty >= MH) return 0;
  char c = maze[ty][tx];
  return c != '#' && c != '-';
}

#define K 2  // outline inset inside a wall tile

static void draw_tile(int tx, int ty) {
  int px = MX0 + tx * TILE, py = MY0 + ty * TILE;
  plat_rect(px, py, TILE, TILE, 0);
  char c = maze[ty][tx];
  if (c == '#') {
    if (walls_hidden) return;
    int u = wopen(tx, ty - 1), l = wopen(tx - 1, ty), d = wopen(tx, ty + 1), r = wopen(tx + 1, ty);
    {  // 25 % grey fill inside the outline (ordered dither, aligned to the screen)
      int gx0 = l ? K + 2 : 0, gx1 = r ? TILE - 3 - K : TILE - 1;
      int gy0 = u ? K + 2 : 0, gy1 = d ? TILE - 3 - K : TILE - 1;
      int cul = !u && !l && wopen(tx - 1, ty - 1), cur = !u && !r && wopen(tx + 1, ty - 1);
      int cdl = !d && !l && wopen(tx - 1, ty + 1), cdr = !d && !r && wopen(tx + 1, ty + 1);
      for (int y = gy0; y <= gy1; y++) {
        int Y = py + y;
        for (int x = gx0 + pmod((Y & 1) * 2 - (px + gx0), 4); x <= gx1; x += 4) {
          int top = y <= K + 1, bot = y >= TILE - 2 - K, lf = x <= K + 1, rt = x >= TILE - 2 - K;
          if ((cul && top && lf) || (cur && top && rt) || (cdl && bot && lf) || (cdr && bot && rt)) continue;
          plat_rect(px + x, Y, 1, 1, 1);
        }
      }
    }
    int y0 = u ? K : 0, y1 = d ? TILE - 1 - K : TILE - 1;
    int x0 = l ? K : 0, x1 = r ? TILE - 1 - K : TILE - 1;
    if (l) plat_rect(px + K, py + y0, 1, y1 - y0 + 1, 1);
    if (r) plat_rect(px + TILE - 1 - K, py + y0, 1, y1 - y0 + 1, 1);
    if (u) plat_rect(px + x0, py + K, x1 - x0 + 1, 1, 1);
    if (d) plat_rect(px + x0, py + TILE - 1 - K, x1 - x0 + 1, 1, 1);
    // inner corners
    if (!u && !l && wopen(tx - 1, ty - 1)) { plat_rect(px, py + K, K + 1, 1, 1); plat_rect(px + K, py, 1, K + 1, 1); }
    if (!u && !r && wopen(tx + 1, ty - 1)) { plat_rect(px + TILE - 1 - K, py + K, K + 1, 1, 1); plat_rect(px + TILE - 1 - K, py, 1, K + 1, 1); }
    if (!d && !l && wopen(tx - 1, ty + 1)) { plat_rect(px, py + TILE - 1 - K, K + 1, 1, 1); plat_rect(px + K, py + TILE - 1 - K, 1, K + 1, 1); }
    if (!d && !r && wopen(tx + 1, ty + 1)) { plat_rect(px + TILE - 1 - K, py + TILE - 1 - K, K + 1, 1, 1); plat_rect(px + TILE - 1 - K, py + TILE - 1 - K, 1, K + 1, 1); }
  } else if (c == '-') {
    if (walls_hidden) return;
    for (int i = 0; i < TILE; i += 2) plat_rect(px + i, py + 4, 1, 2, 1);
  } else if (c == '.') {
    plat_rect(px + 4, py + 4, 2, 2, 1);
  } else if (c == 'o') {
    if (power_vis) {
      plat_rect(px + 3, py + 2, 4, 6, 1);
      plat_rect(px + 2, py + 3, 6, 4, 1);
      plat_rect(px + 4, py + 4, 2, 2, 0);
    }
  }
}

static void draw_area(int x, int y, int w, int h) {  // screen px -> redraw tiles under it
  int tx0 = (x - MX0) / TILE, tx1 = (x + w - 1 - MX0) / TILE;
  int ty0 = (y - MY0) / TILE, ty1 = (y + h - 1 - MY0) / TILE;
  if (x - MX0 < 0) tx0 = 0;
  if (y - MY0 < 0) ty0 = 0;
  if (tx1 >= MW) tx1 = MW - 1;
  if (ty1 >= MH) ty1 = MH - 1;
  for (int ty = ty0; ty <= ty1; ty++)
    for (int tx = tx0; tx <= tx1; tx++) draw_tile(tx, ty);
}

// ---------------------------------------------------------------------------
//  Screen refresh bookkeeping
// ---------------------------------------------------------------------------
static uint8_t dirty[SCR_H];

static void mark(int y, int h) {
  if (y < 0) { h += y; y = 0; }
  if (y + h > SCR_H) h = SCR_H - y;
  for (int i = 0; i < h; i++) dirty[y + i] = 1;
}

static void flush(void) {
  for (int y = 0; y < SCR_H;) {
    if (!dirty[y]) { y++; continue; }
    int y0 = y;
    while (y < SCR_H && dirty[y]) dirty[y++] = 0;
    plat_refresh_lines(y0, y - y0);
  }
}

// ---------------------------------------------------------------------------
//  Side panel + message box
// ---------------------------------------------------------------------------
static void text_c(int y, const char *s, int font, int inv) {
  int w = plat_text_w(s, font);
  int x = PANEL_X + (PANEL_W - w) / 2;
  if (inv) plat_rect(PANEL_X, y, PANEL_W, plat_font_h(font), 1);
  plat_text(x, y, s, font, inv);
}

static void draw_panel(void) {
  char s[16];
  plat_rect(MAZE_PX_W, 0, SCR_W - MAZE_PX_W, SCR_H, 0);
  text_c(4, "BYTE RUN", FONT_SMALL, 1);
  text_c(32, "SCORE", FONT_SMALL, 0);
  utoa10(score, s); text_c(52, s, FONT_SMALL, 0);
  text_c(80, "HIGH", FONT_SMALL, 0);
  utoa10(high, s); text_c(100, s, FONT_SMALL, 0);
  strcpy(s, "LEVEL "); utoa10(level, s + 6); text_c(128, s, FONT_SMALL, 0);
  // reserve lives
  int n = lives - 1; if (n > 6) n = 6;
  int x = PANEL_X + (PANEL_W - (n * 12 - 3)) / 2;
  for (int i = 0; i < n; i++) {
    blit(x + i * 12, 158, HERO_A, 9, 0);
    plat_rect(x + i * 12 + 2, 161, 2, 2, 0);
    plat_rect(x + i * 12 + 5, 161, 2, 2, 0);
  }
  text_c(180, "F1 Sound", FONT_SMALL, 0);
  text_c(202, SND_NAME[sound_mode], FONT_SMALL, sound_on);
}

static void mark_panel_scores(void) {
  int fh = plat_font_h(FONT_SMALL);
  mark(52, fh); mark(100, fh); mark(156, 13);
}

static void draw_msg(const char *s) {
  int w = plat_text_w(s, FONT_BIG) + 16, h = plat_font_h(FONT_BIG) + 8;
  int cx = MX0 + MAZE_PX_W / 2, cy = MY0 + 13 * TILE + TILE / 2;
  int x = cx - w / 2, y = cy - h / 2;
  plat_rect(x - 2, y - 2, w + 4, h + 4, 0);
  plat_rect(x, y, w, h, 1);
  plat_rect(x + 1, y + 1, w - 2, h - 2, 0);
  plat_text(x + 8, y + 4, s, FONT_BIG, 0);
  mark(y - 2, h + 4);
}

// ---------------------------------------------------------------------------
//  Sprite bookkeeping for incremental redraw
// ---------------------------------------------------------------------------
enum { SP_HERO, SP_E0, SP_E1, SP_E2, SP_E3, SP_BONUS, SP_POPUP, NSPR };
typedef struct { int x, y, w, h, vis; } rect_t;
static rect_t prev[NSPR];

static int spx(int sub) { return MX0 + sub / SUB; }
static int spy(int sub) { return MY0 + sub / SUB; }

static void hero_rect(rect_t *r) { r->x = spx(hero.x) - 4; r->y = spy(hero.y) - 4; r->w = 9; r->h = 9; }

static void compute_sprites(rect_t *cur) {
  memset(cur, 0, sizeof(rect_t) * NSPR);
  int playing = state == GS_PLAY || state == GS_READY || state == GS_PAUSE;
  // hero
  hero_rect(&cur[SP_HERO]);
  cur[SP_HERO].vis = (state != GS_TITLE && state != GS_OVER) && !(freeze && popup_t);
  if (state == GS_DYING) cur[SP_HERO].x -= 8, cur[SP_HERO].y -= 8, cur[SP_HERO].w = 25, cur[SP_HERO].h = 25;
  for (int i = 0; i < 4; i++) {
    rect_t *r = &cur[SP_E0 + i];
    r->x = spx(en[i].m.x) - 4; r->y = spy(en[i].m.y) - 4; r->w = 9; r->h = 9;
    r->vis = playing || (state == GS_DYING && timer > SEC(1.6));
  }
  if (bonus_t > 0 && playing) {
    rect_t *r = &cur[SP_BONUS];
    r->x = MX0 + HERO_X * TILE; r->y = MY0 + HERO_Y * TILE; r->w = 9; r->h = 9; r->vis = 1;
  }
  if (popup_t > 0 && playing) {
    rect_t *r = &cur[SP_POPUP];
    int w = draw_num_tiny(0, 0, popup_val, 1);
    r->x = spx(popup_x) - w / 2; r->y = spy(popup_y) - 2; r->w = w; r->h = 5; r->vis = 1;
  }
}

static void draw_sprites(rect_t *cur) {
  if (cur[SP_BONUS].vis) blit(cur[SP_BONUS].x, cur[SP_BONUS].y, CHIP, 9, 1);
  for (int i = 0; i < 4; i++) {
    // draw returning cores first, active enemies on top
    const enemy_t *e = &en[i];
    if (cur[SP_E0 + i].vis && (e->mode == EM_EYES || e->mode == EM_ENTER))
      draw_enemy(e, cur[SP_E0 + i].x, cur[SP_E0 + i].y);
  }
  if (cur[SP_HERO].vis) {
    if (state == GS_DYING) {
      int k = timer > SEC(1.6) ? 0 : (SEC(1.6) - timer) * 9 / SEC(1.4);
      if (k > 9) k = 9;
      draw_hero_dissolve(cur[SP_HERO].x + 8, cur[SP_HERO].y + 8, k);
    } else {
      draw_hero_at(cur[SP_HERO].x, cur[SP_HERO].y, hero.dir < 0 ? hero.ndir : hero.dir,
                   (hero_anim / 3) & 1);
    }
  }
  for (int i = 0; i < 4; i++) {
    const enemy_t *e = &en[i];
    if (cur[SP_E0 + i].vis && !(e->mode == EM_EYES || e->mode == EM_ENTER))
      draw_enemy(e, cur[SP_E0 + i].x, cur[SP_E0 + i].y);
  }
  if (cur[SP_POPUP].vis) {
    rect_t *r = &cur[SP_POPUP];
    MRECT(r->x - 1, r->y - 1, r->w + 2, r->h + 2, 0);
    draw_num_tiny(r->x, r->y, popup_val, 0);
  }
}

// Tiles to repaint this frame (eaten dots, blinking power cells)
static uint8_t tdirty_x[16], tdirty_y[16]; static int tdirty_n;
static void tile_dirty(int tx, int ty) {
  if (tdirty_n < 16) { tdirty_x[tdirty_n] = tx; tdirty_y[tdirty_n] = ty; tdirty_n++; }
}

static int panel_dirty;

static void render_frame(void) {
  rect_t cur[NSPR];
  compute_sprites(cur);
  for (int i = 0; i < NSPR; i++)
    if (prev[i].vis) { draw_area(prev[i].x, prev[i].y, prev[i].w, prev[i].h); mark(prev[i].y, prev[i].h); }
  for (int i = 0; i < tdirty_n; i++) { draw_tile(tdirty_x[i], tdirty_y[i]); mark(MY0 + tdirty_y[i] * TILE, TILE); }
  tdirty_n = 0;
  // anything newly visible must also have its background restored
  for (int i = 0; i < NSPR; i++)
    if (cur[i].vis) { draw_area(cur[i].x, cur[i].y, cur[i].w, cur[i].h); mark(cur[i].y, cur[i].h); }
  draw_sprites(cur);
  memcpy(prev, cur, sizeof(prev));
  if (panel_dirty) { draw_panel(); mark_panel_scores(); panel_dirty = 0; }
  flush();
}

static void render_title(void);

static void render_full(void) {
  if (state == GS_TITLE) { render_title(); return; }
  plat_clear();
  for (int ty = 0; ty < MH; ty++)
    for (int tx = 0; tx < MW; tx++) draw_tile(tx, ty);
  draw_panel();
  panel_dirty = 0;
  tdirty_n = 0;
  rect_t cur[NSPR];
  compute_sprites(cur);
  draw_sprites(cur);
  memcpy(prev, cur, sizeof(prev));
  if (state == GS_READY) draw_msg(level == 1 && score == 0 ? "READY!" : "GET SET!");
  if (state == GS_PAUSE) draw_msg("PAUSED");
  if (state == GS_OVER)  draw_msg("GAME OVER");
  memset(dirty, 0, sizeof(dirty));
  plat_refresh_all();
}

void game_redraw(void) { render_full(); }

// ---------------------------------------------------------------------------
//  Title screen
// ---------------------------------------------------------------------------
static void render_title(void) {
  char s[40];
  plat_clear();
  int fh = plat_font_h(FONT_SMALL), bh = plat_font_h(FONT_BIG);
  plat_rect(0, 0, SCR_W, bh + 6, 1);
  const char *t = "B Y T E    R U N";
  plat_text((SCR_W - plat_text_w(t, FONT_BIG)) / 2, 3, t, FONT_BIG, 1);

  int y = bh + 14, c1 = 14, c2 = 210, ic = fh / 2 - 4;
  walls_hidden = 0;
  // legend: two columns
  draw_hero_at(c1, y + ic, 3, 0);
  plat_text(c1 + 18, y, "You (Byte)", FONT_SMALL, 0);
  blit(c2, y + ic, GL_A, 9, 0);
  plat_rect(c2 + 2, y + ic + 3, 2, 2, 0); plat_rect(c2 + 5, y + ic + 3, 2, 2, 0);
  plat_text(c2 + 18, y, "Glitch: avoid!", FONT_SMALL, 0);
  y += fh + 3;
  plat_rect(c1 + 3, y + ic + 3, 2, 2, 1);
  plat_text(c1 + 18, y, "Bit  10", FONT_SMALL, 0);
  plat_rect(c2 + 2, y + ic + 1, 4, 6, 1); plat_rect(c2 + 1, y + ic + 2, 6, 4, 1);
  plat_rect(c2 + 3, y + ic + 3, 2, 2, 0);
  plat_text(c2 + 18, y, "Power cell  50", FONT_SMALL, 0);
  y += fh + 3;
  blit(c1, y + ic, GL_F, 9, 0);
  plat_text(c1 + 18, y, "Zap  200-1600", FONT_SMALL, 0);
  blit(c2, y + ic, CHIP, 9, 0);
  plat_text(c2 + 18, y, "Bonus chip", FONT_SMALL, 0);
  y += fh + 8;

  plat_rect(10, y - 4, SCR_W - 20, 1, 1);
  plat_text(c1, y, "Move: 8 4 6 2 (or 5), arrows", FONT_SMALL, 0); y += fh + 1;
  strcpy(s, "F1: Sound "); strcat(s, SND_NAME[sound_mode]);
  plat_text(c1, y, s, FONT_SMALL, 0);
  plat_text(c2, y, "EXIT: Pause", FONT_SMALL, 0); y += fh + 1;
  plat_text(c1, y, "SHIFT 0: Setup", FONT_SMALL, 0);
  plat_text(c2, y, "SHIFT EXIT: Off", FONT_SMALL, 0); y += fh + 6;

  strcpy(s, "High score: "); utoa10(high, s + strlen(s));
  plat_text(c1, y, s, FONT_SMALL, 0);
  const char *st = " ENTER: start ";
  int w = plat_text_w(st, FONT_SMALL);
  plat_rect(SCR_W - w - 14, y - 2, w + 4, fh + 4, 1);
  plat_text(SCR_W - w - 12, y, st, FONT_SMALL, 1);
  memset(dirty, 0, sizeof(dirty));
  plat_refresh_all();
}

// ---------------------------------------------------------------------------
//  Game setup
// ---------------------------------------------------------------------------
static void place_actors(void) {
  hero.x = HERO_X * TS + HALF; hero.y = HERO_Y * TS + HALF;
  hero.dir = 1; hero.ndir = 1; hero_anim = 0;
  static const int hx[4] = { 15, 15, 13, 17 };
  static const int hy[4] = { OUT_Y, PEN_Y, PEN_Y, PEN_Y };
  for (int i = 0; i < 4; i++) {
    enemy_t *e = &en[i];
    memset(e, 0, sizeof(*e));
    e->id = i;
    e->home_x = hx[i] * TS + HALF; e->home_y = hy[i] * TS + HALF;
    e->m.x = e->home_x; e->m.y = e->home_y;
    e->m.dir = i == 0 ? 1 : 0;
    e->mode = i == 0 ? EM_ACTIVE : EM_PEN;
  }
  fright_timer = 0; chain = 0; freeze = 0; popup_t = 0;
  mode_phase = 0; mode_timer = 0; scatter = 1;
  life_ticks = 0; dots_eaten_life = 0;
  memset(prev, 0, sizeof(prev));
}

static void start_level(void) {
  for (int y = 0; y < MH; y++) memcpy(maze[y], maze_src[y], MW);
  dots_left = 0;
  for (int y = 0; y < MH; y++)
    for (int x = 0; x < MW; x++)
      if (maze[y][x] == '.' || maze[y][x] == 'o') dots_left++;
  dots_eaten_level = 0;
  bonus_t = 0; bonus_shown = 0;
  walls_hidden = 0; power_vis = 1;
  place_actors();
}

static void new_game(void) {
  score = 0; lives = 3; level = 1; next_life = 10000;
  rng ^= plat_ms() * 2654435761u; if (!rng) rng = 1;
  start_level();
  state = GS_READY; timer = SEC(4.6);   // room for the start-up tune
  render_full();
  sfx(NOTES(SND_START), 9);
}

// ---------------------------------------------------------------------------
//  Movement
// ---------------------------------------------------------------------------
static int at_center(const mover_t *m) {
  return pmod(m->x - HALF, TS) == 0 && pmod(m->y - HALF, TS) == 0;
}

typedef int (*choose_fn)(void *ctx);

static void advance(mover_t *m, int dist, choose_fn choose, void *ctx) {
  int guard = 6;
  while (dist > 0 && guard--) {
    if (at_center(m)) {
      int nd = choose(ctx);
      if (nd < 0) { m->dir = -1; return; }
      m->dir = nd;
    }
    if (m->dir < 0) return;
    int off = pmod(((m->dir & 1) ? m->x : m->y) - HALF, TS);
    int d = (m->dir >= 2) ? TS - off : (off ? off : TS);
    int step = dist < d ? dist : d;
    m->x = pmod(m->x + DX[m->dir] * step, MW * TS);
    m->y += DY[m->dir] * step;
    dist -= step;
  }
}

static int hero_choose(void *ctx) {
  (void)ctx;
  int tx = hero.x / TS, ty = hero.y / TS;
  if (hero.ndir >= 0 && passable(tx + DX[hero.ndir], ty + DY[hero.ndir])) return hero.ndir;
  if (hero.dir >= 0 && passable(tx + DX[hero.dir], ty + DY[hero.dir])) return hero.dir;
  return -1;
}

static void enemy_target(const enemy_t *e, int *tx, int *ty) {
  int hx = hero.x / TS, hy = hero.y / TS;
  int hd = hero.dir >= 0 ? hero.dir : (hero.ndir >= 0 ? hero.ndir : 1);
  if (e->mode == EM_EYES) { *tx = DOOR_X; *ty = OUT_Y; return; }
  if (scatter) {
    static const int cx[4] = { MW - 2, 1, MW - 1, 0 };
    static const int cy[4] = { -2, -2, MH + 1, MH + 1 };
    *tx = cx[e->id]; *ty = cy[e->id];
    return;
  }
  switch (e->id) {
    case 0: *tx = hx; *ty = hy; break;                              // chaser
    case 1: *tx = hx + 4 * DX[hd]; *ty = hy + 4 * DY[hd]; break;     // ambusher
    case 2: {                                                       // flanker
      int ax = hx + 2 * DX[hd], ay = hy + 2 * DY[hd];
      int bx = en[0].m.x / TS, by = en[0].m.y / TS;
      *tx = 2 * ax - bx; *ty = 2 * ay - by;
    } break;
    default: {                                                      // wanderer
      int ex = e->m.x / TS, ey = e->m.y / TS;
      int d2 = (ex - hx) * (ex - hx) + (ey - hy) * (ey - hy);
      if (d2 > 64) { *tx = hx; *ty = hy; } else { *tx = 0; *ty = MH + 1; }
    }
  }
}

static int enemy_choose(void *ctx) {
  enemy_t *e = (enemy_t *)ctx;
  int tx = e->m.x / TS, ty = e->m.y / TS;
  if (e->mode == EM_EYES && tx == DOOR_X && ty == OUT_Y) { e->mode = EM_ENTER; return -1; }
  int opts[4], n = 0;
  for (int d = 0; d < 4; d++) {
    if (e->m.dir >= 0 && d == OPP(e->m.dir)) continue;
    if (passable(tx + DX[d], ty + DY[d])) opts[n++] = d;
  }
  if (n == 0) return e->m.dir >= 0 ? OPP(e->m.dir) : -1;
  if (e->fright) return opts[rnd() % n];
  int gx, gy; enemy_target(e, &gx, &gy);
  int best = opts[0], bd = 0x7fffffff;
  for (int i = 0; i < n; i++) {
    int nx = tx + DX[opts[i]] - gx, ny = ty + DY[opts[i]] - gy;
    int dd = nx * nx + ny * ny;
    if (dd < bd) { bd = dd; best = opts[i]; }
  }
  return best;
}

// Straight-line move toward (x,y) ignoring walls; returns 1 when arrived
static int move_to(mover_t *m, int x, int y, int sp) {
  if (m->x != x) {
    int d = x - m->x; if (d > sp) d = sp; if (d < -sp) d = -sp;
    m->x += d; m->dir = d > 0 ? 3 : 1;
    return 0;
  }
  if (m->y != y) {
    int d = y - m->y; if (d > sp) d = sp; if (d < -sp) d = -sp;
    m->y += d; m->dir = d > 0 ? 2 : 0;
    return 0;
  }
  return 1;
}

static int sp_hero(void)  { int s = 26 + 2 * level; return s > 36 ? 36 : s; }
static int sp_enemy(void) { int s = 24 + 2 * level; return s > 35 ? 35 : s; }

static void enemy_update(enemy_t *e) {
  switch (e->mode) {
    case EM_PEN: {
      // bob up and down while waiting
      int ph = (ticks + e->id * 7) % 24;
      int off = (ph < 12 ? ph : 24 - ph) - 6;
      e->m.y = e->home_y + off * SUB / 3;
      e->m.dir = ph < 12 ? 2 : 0;
      static const int rel_t[4] = { 0, 0, SEC(4), SEC(8) };
      static const int rel_d[4] = { 0, 0, 30, 70 };
      int lt = rel_t[e->id] - (level - 1) * SEC(1); if (lt < 0) lt = 0;
      if (life_ticks >= lt || dots_eaten_life >= rel_d[e->id]) e->mode = EM_LEAVE;
    } break;
    case EM_LEAVE:
      if (move_to(&e->m, DOOR_X * TS + HALF, OUT_Y * TS + HALF, e->fright ? 10 : 16)) {
        e->mode = EM_ACTIVE;
        e->m.dir = (rnd() & 1) ? 1 : 3;
      }
      break;
    case EM_ACTIVE: {
      int sp = e->fright ? 16 : sp_enemy();
      int ty = e->m.y / TS, tx = e->m.x / TS;
      if (ty == 11 && (tx < 5 || tx > MW - 6)) sp = 14;  // slow in the tunnel
      advance(&e->m, sp, enemy_choose, e);
    } break;
    case EM_EYES:
      advance(&e->m, 56, enemy_choose, e);
      break;
    case EM_ENTER:
      if (move_to(&e->m, DOOR_X * TS + HALF, PEN_Y * TS + HALF, 40)) {
        e->mode = EM_LEAVE; e->fright = 0;
      }
      break;
  }
}

// ---------------------------------------------------------------------------
//  Play logic
// ---------------------------------------------------------------------------
static void add_score(uint32_t v) {
  score += v;
  if (score >= next_life) {
    next_life += 10000;  // an extra robot every 10000 points
    lives++;
    sfx(NOTES(SND_LIFE), 6);
  }
  if (score > high) { high = score; cfg_dirty = 1; }
  panel_dirty = 1;
}

static void reverse_active(void) {
  for (int i = 0; i < 4; i++)
    if (en[i].mode == EM_ACTIVE && en[i].m.dir >= 0) en[i].m.dir = OPP(en[i].m.dir);
}

static int fright_len(void) { int t = SEC(6) - (level - 1) * SEC(0.6); return t < SEC(1.5) ? SEC(1.5) : t; }

static void eat_here(void) {
  int tx = hero.x / TS, ty = hero.y / TS;
  char c = tile_at(tx, ty);
  tx = pmod(tx, MW);
  if (c == '.' || c == 'o') {
    maze[ty][tx] = ' ';
    tile_dirty(tx, ty);
    dots_left--; dots_eaten_level++; dots_eaten_life++;
    if (c == '.') {
      add_score(10);
      if (dots_left & 1) sfx(NOTES(SND_DOT1), 1); else sfx(NOTES(SND_DOT2), 1);
    } else {
      add_score(50);
      fright_timer = fright_len(); chain = 0;
      for (int i = 0; i < 4; i++)
        if (en[i].mode == EM_ACTIVE || en[i].mode == EM_PEN || en[i].mode == EM_LEAVE) en[i].fright = 1;
      reverse_active();
      sfx(NOTES(SND_POWER), 4);
    }
    if (dots_eaten_level == 70 || dots_eaten_level == 170) { bonus_t = SEC(9); }
  }
  if (bonus_t > 0 && tx == HERO_X && ty == HERO_Y) {
    static const uint16_t bv[8] = { 100, 300, 500, 700, 1000, 2000, 3000, 5000 };
    int v = bv[level - 1 < 7 ? level - 1 : 7];
    add_score(v);
    bonus_t = 0;
    popup_val = v; popup_x = HERO_X * TS + HALF; popup_y = HERO_Y * TS + HALF; popup_t = SEC(1.5);
    sfx(NOTES(SND_BONUS), 5);
  }
}

static int collide(void) {
  for (int i = 0; i < 4; i++) {
    enemy_t *e = &en[i];
    if (e->mode != EM_ACTIVE && e->mode != EM_LEAVE) continue;
    int dx = e->m.x - hero.x, dy = e->m.y - hero.y;
    if (dx > MW * TS / 2) dx -= MW * TS;
    if (dx < -MW * TS / 2) dx += MW * TS;
    if (dx < 0) dx = -dx;
    if (dy < 0) dy = -dy;
    if (dx < 6 * SUB && dy < 6 * SUB) {
      if (e->fright) {
        e->fright = 0;
        int was_leaving = e->mode == EM_LEAVE;
        e->mode = was_leaving ? EM_ENTER : EM_EYES;
        uint32_t v = 200u << chain; if (chain < 3) chain++;
        add_score(v);
        popup_val = v; popup_x = e->m.x; popup_y = e->m.y; popup_t = SEC(0.8);
        freeze = SEC(0.5);
        // snap to tile grid so the core can navigate home
        if (!was_leaving) { e->m.x = (e->m.x / TS) * TS + HALF; e->m.y = (e->m.y / TS) * TS + HALF; }
        e->m.dir = -1;
        sfx(NOTES(SND_EAT), 7);
      } else {
        return 1;
      }
    }
  }
  return 0;
}

static const int mode_sched[] = { SEC(7), SEC(20), SEC(7), SEC(20), SEC(5), -1 };

static void play_tick(void) {
  if (popup_t > 0) popup_t--;
  if (freeze > 0) { freeze--; return; }
  life_ticks++;

  // scatter / chase schedule (paused while powered up)
  if (fright_timer > 0) {
    if (--fright_timer == 0) for (int i = 0; i < 4; i++) en[i].fright = 0;
  } else if (mode_sched[mode_phase] >= 0 && ++mode_timer >= mode_sched[mode_phase]) {
    mode_phase++; mode_timer = 0; scatter = !scatter;
    reverse_active();
  }
  if (bonus_t > 0) bonus_t--;

  // power cell blinking
  if ((ticks % 8) == 0) {
    power_vis = !power_vis;
    for (int y = 0; y < MH; y++)
      for (int x = 0; x < MW; x++)
        if (maze[y][x] == 'o') tile_dirty(x, y);
  }

  // hero
  int ox = hero.x, oy = hero.y;
  advance(&hero, sp_hero(), hero_choose, 0);
  if (hero.x != ox || hero.y != oy) hero_anim++;
  eat_here();
  if (collide()) goto die;

  for (int i = 0; i < 4; i++) enemy_update(&en[i]);
  if (collide()) goto die;

  if (dots_left == 0) {
    state = GS_CLEAR; timer = SEC(2.4);
    sfx(NOTES(SND_CLEAR), 8);
  }
  return;

die:
  state = GS_DYING; timer = SEC(2.6);
  fright_timer = 0;
  sfx_stop();
}

// ---------------------------------------------------------------------------
//  Public interface
// ---------------------------------------------------------------------------
void game_init(void) {
  cfg_t c;
  high = 0; sound_mode = 2;
  if (plat_load_cfg(&c) && c.magic == CFG_MAGIC) { high = c.high; sound_mode = c.sound <= 2 ? (int)c.sound : 2; }
  sound_on = sound_mode > 0;
  level = 1; lives = 3;
  start_level();
  state = GS_TITLE;
  render_full();
}

void game_save(void) {
  if (!cfg_dirty) return;
  cfg_t c = { CFG_MAGIC, high, (uint32_t)sound_mode };
  plat_save_cfg(&c);
  cfg_dirty = 0;
}

static void toggle_sound(void) {
  sound_mode = sound_mode == 0 ? 2 : sound_mode - 1;   // ALL -> FX -> OFF -> ALL
  sound_on = sound_mode > 0;
  cfg_dirty = 1;
  sfx_stop();
  if (sound_on) sfx(NOTES(SND_ON), 9);
  if (state == GS_TITLE) render_title();
  else { draw_panel(); mark(202, plat_font_h(FONT_SMALL)); flush(); }
}

void game_pause(void) {
  if (state == GS_PLAY || state == GS_READY) {
    prev_state = state;
    state = GS_PAUSE;
    sfx_stop();
    game_save();
    render_full();
  }
}

void game_on_wake(void) {
  game_pause();
  render_full();
}

void game_key(int k) {
  if (k == GK_SOUND) { toggle_sound(); return; }
  switch (state) {
    case GS_TITLE:
      if (k == GK_START || (k >= GK_UP && k <= GK_RIGHT)) new_game();
      break;
    case GS_READY:
    case GS_PLAY:
      if (k >= GK_UP && k <= GK_RIGHT) {
        int d = k - GK_UP;  // GK order matches dir order (up,left,down,right)
        hero.ndir = d;
        if (state == GS_PLAY && hero.dir >= 0 && d == OPP(hero.dir)) hero.dir = d;
      } else if (k == GK_PAUSE) {
        game_pause();
      }
      break;
    case GS_PAUSE:
      state = prev_state;
      render_full();
      break;
    case GS_OVER:
      if (timer == 0) { state = GS_TITLE; render_full(); }
      break;
    default:
      break;
  }
}

int game_tick(void) {
  ticks++;
  switch (state) {
    case GS_TITLE:
      return sfx_busy();
    case GS_READY:
      if (--timer <= 0) { state = GS_PLAY; render_full(); }
      return 1;
    case GS_PLAY:
      play_tick();
      render_frame();
      if (bg_hold > 0) bg_hold--;
      else if (sound_mode == 2 && !sq && !freeze && state == GS_PLAY) {
        int eyes = 0;
        for (int i = 0; i < 4; i++) eyes |= en[i].mode == EM_EYES || en[i].mode == EM_ENTER;
        if (eyes) sfx(NOTES(BG_EYES), 0);
        else if (fright_timer > 0) sfx(NOTES(BG_FRIGHT), 0);
        else {  // low pulse that speeds up as the maze empties
          BG_HUM[0] = (note_t)S(260, 400, 60);
          BG_HUM[1] = (note_t)R(80 + dots_left * 2);
          sfx(NOTES(BG_HUM), 0);
        }
      }
      return 1;
    case GS_DYING:
      timer--;
      if (timer == SEC(1.6)) sfx(NOTES(SND_DEATH), 10);
      if (timer <= 0) {
        lives--;
        panel_dirty = 1;
        if (lives <= 0) {
          state = GS_OVER; timer = SEC(1.5);
          game_save();
          render_full();
        } else {
          place_actors();
          state = GS_READY; timer = SEC(1.6);
          render_full();
        }
        return 1;
      }
      render_frame();
      return 1;
    case GS_CLEAR:
      timer--;
      if (timer < SEC(2.0) && (timer % 6) == 0) {
        walls_hidden = !walls_hidden;
        state = GS_CLEAR;
        render_full();
      }
      if (timer <= 0) {
        level++;
        start_level();
        state = GS_READY; timer = SEC(1.6);
        render_full();
      }
      return 1;
    case GS_OVER:
      if (timer > 0) timer--;
      return timer > 0 || sfx_busy();
    case GS_PAUSE:
      return sfx_busy();
  }
  return 0;
}

#ifdef SIM
// Test hooks for the desktop simulator
int sim_state(void) { return state; }
int sim_level(void) { return level; }
int sim_lives(void) { return lives; }
uint32_t sim_score(void) { return score; }
int sim_dots(void) { return dots_left; }
int sim_hero_tile(int *x, int *y) { *x = hero.x / TS; *y = hero.y / TS; return at_center(&hero); }
int sim_hero_dir(void) { return hero.dir; }
char sim_tile(int x, int y) { return tile_at(x, y); }
int sim_passable(int x, int y) { return passable(x, y); }
int sim_enemy(int i, int *x, int *y) { *x = en[i].m.x / TS; *y = en[i].m.y / TS; return en[i].mode * 2 + en[i].fright; }
#endif
