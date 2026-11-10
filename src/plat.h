// SPDX-License-Identifier: MIT
// Copyright (c) 2026 rudibr
// plat.h - tiny platform layer used by the game core.
// Implemented by dm42_main.c (calculator) and sim/sim.c (desktop test harness).
#ifndef PLAT_H
#define PLAT_H

#include <stdint.h>

#define SCR_W 400
#define SCR_H 240

#define FONT_SMALL 0   // DMCP t20
#define FONT_BIG   1   // DMCP t24

void     plat_clear(void);                                  // whole buffer white
void     plat_rect(int x, int y, int w, int h, int black);  // filled rectangle
void     plat_text(int x, int y, const char *s, int font, int inv);
int      plat_text_w(const char *s, int font);
int      plat_font_h(int font);
void     plat_refresh_lines(int y, int n);                  // push lines to LCD
void     plat_refresh_all(void);
void     plat_tone(int hz);                                 // 0 = silence
uint32_t plat_ms(void);

// Persistent settings (high score + sound on/off)
typedef struct {
  uint32_t magic;
  uint32_t high;
  uint32_t sound;
} cfg_t;
#define CFG_MAGIC 0x42595432u  // "BYT2"

int  plat_load_cfg(cfg_t *c);   // 1 = ok
void plat_save_cfg(const cfg_t *c);

#endif
