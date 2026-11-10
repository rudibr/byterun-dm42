// SPDX-License-Identifier: MIT
// Copyright (c) 2026 rudibr
// Desktop test harness: runs the game core with a virtual clock, an autopilot
// player and a framebuffer that can be dumped to PGM files.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "../src/plat.h"
#include "../src/game.h"
#include "simfont.h"

static uint8_t fb[SCR_H][SCR_W];
static uint32_t now;
static long refreshed_lines, refresh_calls, tone_changes;
static int cur_tone;

void plat_clear(void) { memset(fb, 0, sizeof fb); }
void plat_rect(int x, int y, int w, int h, int b) {
  if (x < 0 || y < 0 || x + w > SCR_W || y + h > SCR_H || w <= 0 || h <= 0) {
    fprintf(stderr, "BAD RECT %d %d %d %d\n", x, y, w, h); abort();
  }
  for (int j = y; j < y + h; j++) for (int i = x; i < x + w; i++) fb[j][i] = b;
}
static int glyph(int f, int c, int x, int y, int inv, int draw) {
  if (c < 32 || c > 126) c = '?';
  int w = f ? big_w[c - 32] : small_w[c - 32], h = f ? big_h : small_h;
  if (draw)
    for (int j = 0; j < h; j++)
      for (int i = 0; i < w; i++) {
        int on = ((f ? big_g[c - 32][j] : small_g[c - 32][j]) >> i) & 1;
        if (x + i < SCR_W && y + j < SCR_H) fb[y + j][x + i] = inv ? !on : on;
      }
  return w;
}
void plat_text(int x, int y, const char *s, int f, int inv) { for (; *s; s++) x += glyph(f, *s, x, y, inv, 1); }
int plat_text_w(const char *s, int f) { int w = 0; for (; *s; s++) w += glyph(f, *s, 0, 0, 0, 0); return w; }
int plat_font_h(int f) { return f ? big_h : small_h; }
void plat_refresh_lines(int y, int n) { refreshed_lines += n; refresh_calls++; }
void plat_refresh_all(void) { refreshed_lines += SCR_H; refresh_calls++; }
void plat_tone(int hz) { if (hz != cur_tone) tone_changes++; cur_tone = hz; }
uint32_t plat_ms(void) { return now; }
int plat_load_cfg(cfg_t *c) { (void)c; return 0; }
void plat_save_cfg(const cfg_t *c) { (void)c; }

// hooks from game.c
int sim_state(void); int sim_level(void); int sim_lives(void); uint32_t sim_score(void);
int sim_dots(void); int sim_hero_tile(int *x, int *y); int sim_hero_dir(void);
char sim_tile(int x, int y); int sim_passable(int x, int y); int sim_enemy(int i, int *x, int *y);

static void dump(const char *name) {
  FILE *f = fopen(name, "wb");
  fprintf(f, "P5 %d %d 255\n", SCR_W, SCR_H);
  for (int y = 0; y < SCR_H; y++) for (int x = 0; x < SCR_W; x++) fputc(fb[y][x] ? 0 : 255, f);
  fclose(f);
}

static const int DX[4] = {0, -1, 0, 1}, DY[4] = {-1, 0, 1, 0};

// BFS autopilot: head for nearest dot (or frightened enemy), avoid dangerous enemies
static int pilot(int skill) {
  int hx, hy; sim_hero_tile(&hx, &hy);
  static int dist[23][31], first[23][31];
  int danger[23][31]; memset(danger, 0, sizeof danger);
  int target[23][31]; memset(target, 0, sizeof target);
  for (int i = 0; i < 4; i++) {
    int ex, ey, m = sim_enemy(i, &ex, &ey);
    int mode = m / 2, fr = m & 1;
    if (mode != 2 && mode != 1) continue;
    if (fr) { target[ey][((ex % 31) + 31) % 31] = 1; continue; }
    for (int dy = -skill; dy <= skill; dy++) for (int dx = -skill; dx <= skill; dx++) {
      int y = ey + dy, x = ((ex + dx) % 31 + 31) % 31;
      if (y >= 0 && y < 23 && abs(dx) + abs(dy) <= skill) danger[y][x] = 1;
    }
  }
  for (int y = 0; y < 23; y++) for (int x = 0; x < 31; x++) dist[y][x] = -1;
  int qx[800], qy[800], h = 0, t = 0;
  dist[hy][hx] = 0; first[hy][hx] = -1; qx[t] = hx; qy[t++] = hy;
  while (h < t) {
    int x = qx[h], y = qy[h++];
    char c = sim_tile(x, y);
    if ((x != hx || y != hy) && (c == '.' || c == 'o' || target[y][x])) return first[y][x];
    for (int d = 0; d < 4; d++) {
      int nx = ((x + DX[d]) % 31 + 31) % 31, ny = y + DY[d];
      if (!sim_passable(nx, ny) || dist[ny][nx] >= 0 || danger[ny][nx]) continue;
      dist[ny][nx] = dist[y][x] + 1; first[ny][nx] = first[y][x] < 0 ? d : first[y][x];
      qx[t] = nx; qy[t++] = ny;
    }
  }
  return rand() % 4;
}

int main(int argc, char **argv) {
  int maxt = argc > 1 ? atoi(argv[1]) : 20000;
  int skill = argc > 2 ? atoi(argv[2]) : 2;
  srand(7);
  game_init();
  dump("f_title.pgm");
  game_key(GK_START);
  int last_state = -1, last_level = 1, last_lives = 3, shots = 0;
  long frames_play = 0, lines_play = 0;
  for (int t = 0; t < maxt; t++) {
    int d = pilot(skill);
    if (d >= 0) game_key(GK_UP + d);
    if (t == 900) { game_key(GK_SOUND); game_key(GK_SOUND); game_key(GK_SOUND); }
    long l0 = refreshed_lines;
    int act = game_tick();
    for (int k = 0; k < TICK_MS; k += 4) { now += 4; game_sfx_update(); }
    int st = sim_state();
    if (st == 2) { frames_play++; lines_play += refreshed_lines - l0; }
    if (st != last_state) {
      printf("t=%5d state %d -> %d  score=%u level=%d lives=%d dots=%d\n", t, last_state, st, sim_score(), sim_level(), sim_lives(), sim_dots());
      last_state = st;
    }
    if (getenv("REC0") && t >= atoi(getenv("REC0")) && t < atoi(getenv("REC1"))) {
      char n[64]; sprintf(n, "rec/r%05d.pgm", t); dump(n);
    }
    if (t == 60 || t == 400 || t == 1500 || (st == 3 && shots < 1 && t % 5 == 0 && ++shots)) {
      char n[64]; sprintf(n, "f_%05d.pgm", t); dump(n);
    }
    { static int got_f = 0, got_e = 0; int ex, ey;
      for (int i = 0; i < 4; i++) { int m = sim_enemy(i, &ex, &ey);
        if ((m & 1) && !got_f && st == 2 && ++got_f) dump("f_fright.pgm");
        if (m / 2 == 3 && !got_e && st == 2 && ++got_e) dump("f_eaten.pgm"); } }
    if (st == 2 && t % 97 == 0) {   // trail check: incremental buffer must equal a full redraw
      static uint8_t a[SCR_H][SCR_W]; memcpy(a, fb, sizeof fb);
      game_redraw();
      if (memcmp(a, fb, sizeof fb)) { int diff = 0; for (int y = 0; y < SCR_H; y++) for (int x = 0; x < SCR_W; x++) diff += a[y][x] != fb[y][x];
        printf("MISMATCH t=%d (%d px)\n", t, diff); dump("f_mm_full.pgm"); memcpy(fb, a, sizeof fb); dump("f_mm_inc.pgm"); memcpy(a, fb, sizeof fb); game_redraw(); }
    }
    if (sim_level() != last_level) { last_level = sim_level(); }
    if (sim_lives() != last_lives) last_lives = sim_lives();
    if (st == 5 && !act) { printf("game over at t=%d score=%u level=%d\n", t, sim_score(), sim_level()); dump("f_over.pgm"); break; }
  }
  printf("avg refreshed lines per play frame: %.1f; tone changes %ld\n", frames_play ? (double)lines_play / frames_play : 0, tone_changes);
  return 0;
}
