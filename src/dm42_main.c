// SPDX-License-Identifier: MIT
// Copyright (c) 2026 rudibr
// dm42_main.c - DMCP glue for BYTE RUN (SwissMicros DM42)
//
// Keys:   8/4/6/2 (or 5) and the up/down arrows move
//         ENTER starts, EXIT pauses, F1 toggles sound
//         SHIFT 0 opens the setup menu (load other programs, etc.)
//         SHIFT EXIT turns the calculator off

#include <string.h>
#include <main.h>
#include <dmcp.h>
#include <menu.h>

#include "plat.h"
#include "game.h"

// ---------------------------------------------------------------------------
//  Platform implementation
// ---------------------------------------------------------------------------
static disp_stat_t font_save[2];

static disp_stat_t *fnt(int f) { return f == FONT_BIG ? t24 : t20; }

static void fonts_setup(void) {
  font_save[0] = *t20; font_save[1] = *t24;
  for (int i = 0; i < 2; i++) {
    disp_stat_t *d = fnt(i);
    d->lnfill = 0; d->newln = 0; d->bgfill = 0; d->inv = 0;
    d->xoffs = 0; d->y_top_grd = 0; d->ya = 0; d->yb = 0;
  }
}

static void fonts_restore(void) { *t20 = font_save[0]; *t24 = font_save[1]; }

void plat_clear(void) { lcd_clear_buf(); }

void plat_rect(int x, int y, int w, int h, int black) {
  if (w > 0 && h > 0) lcd_fill_rect(x, y, w, h, black ? 1 : 0);
}

void plat_text(int x, int y, const char *s, int font, int inv) {
  disp_stat_t *d = fnt(font);
  lcd_setXY(d, x, y);
  // Normal text: no background fill (the game clears the area itself);
  // inverted text: fill so the glyphs come out white on black.
  d->inv = inv ? 1 : 0;
  d->bgfill = inv ? 1 : 0;
  lcd_writeText(d, s);
  d->inv = 0;
  d->bgfill = 0;
}

int plat_text_w(const char *s, int font) { return lcd_textWidth(fnt(font), s); }
int plat_font_h(int font) { return lcd_lineHeight(fnt(font)); }

void plat_refresh_lines(int y, int n) { lcd_refresh_lines(y, n); }
void plat_refresh_all(void) { lcd_refresh(); }

static int tone_on;
void plat_tone(int hz) {
  if (hz > 0) { start_buzzer_freq((uint32_t)hz * 1000); tone_on = 1; }  // DMCP takes mHz
  else if (tone_on) { stop_buzzer(); tone_on = 0; }
}

uint32_t plat_ms(void) { return sys_current_ms(); }

#define CFG_FILE "/BYTERUN.CFG"
static FIL cfg_fp;

int plat_load_cfg(cfg_t *c) {
  UINT br = 0;
  if (!sys_disk_ok()) return 0;
  if (f_open(&cfg_fp, CFG_FILE, FA_READ) != FR_OK) return 0;
  FRESULT r = f_read(&cfg_fp, c, sizeof(*c), &br);
  f_close(&cfg_fp);
  return r == FR_OK && br == sizeof(*c);
}

void plat_save_cfg(const cfg_t *c) {
  UINT bw = 0;
  if (!sys_disk_ok()) return;
  sys_disk_write_enable(1);
  if (f_open(&cfg_fp, CFG_FILE, FA_WRITE | FA_CREATE_ALWAYS) == FR_OK) {
    f_write(&cfg_fp, c, sizeof(*c), &bw);
    f_close(&cfg_fp);
  }
  sys_disk_write_enable(0);
}

// ---------------------------------------------------------------------------
//  Key mapping
// ---------------------------------------------------------------------------
static int shift;

static int map_key(int k) {
  switch (k) {
    case KEY_8: case KEY_UP:                return GK_UP;
    case KEY_4:                             return GK_LEFT;
    case KEY_2: case KEY_5: case KEY_DOWN:  return GK_DOWN;
    case KEY_6:                             return GK_RIGHT;
    case KEY_ENTER:                         return GK_START;
    case KEY_F1:                            return GK_SOUND;
    case KEY_EXIT:                          return GK_PAUSE;
    default:                                return GK_OTHER;
  }
}

static void run_setup_menu(void) {
  game_pause();
  plat_tone(0);
  fonts_restore();
  SET_ST(STAT_MENU);
  handle_menu(&MID_MENU, MENU_RESET, 0);
  CLR_ST(STAT_MENU);
  wait_for_key_release(-1);
  fonts_setup();
  game_redraw();
}

static void handle_key(int k) {
  if (k == KEY_SCREENSHOT) { create_screenshot(1); return; }
  if (k == KEY_DOUBLE_RELEASE) return;
#ifdef KEY_SETUP_MENU
  if (k == KEY_SETUP_MENU) { run_setup_menu(); shift = 0; return; }
#endif
  if (k == KEY_SHIFT) { shift = !shift; return; }
  if (shift) {
    shift = 0;
    if (k == KEY_EXIT) { game_pause(); game_save(); SET_ST(STAT_PGM_END); return; }
    if (k == KEY_0)    { run_setup_menu(); return; }
  }
  game_key(map_key(k));
}

// ---------------------------------------------------------------------------
//  Program entry
// ---------------------------------------------------------------------------
void program_main(void) {
  run_menu_item_app = run_menu_item;
  menu_line_str_app = menu_line_str;

  fonts_setup();
  game_init();

  int active = 0;
  uint32_t next = sys_current_ms();

  for (;;) {
    // Idle (title/pause/game over): sleep until a key arrives, like the SDK demo
    if ((ST(STAT_PGM_END) && ST(STAT_SUSPENDED)) || (!ST(STAT_PGM_END) && !active && key_empty())) {
      CLR_ST(STAT_RUNNING);
      sys_sleep();
    }

    if (ST(STAT_PGM_END) || ST(STAT_SUSPENDED)) {
      if (!ST(STAT_SUSPENDED)) {
        plat_tone(0);
        game_pause();
        game_save();
        lcd_set_buf_cleared(0);
        fonts_restore();
        draw_power_off_image(1);
        fonts_setup();
        LCD_power_off(0);
        SET_ST(STAT_SUSPENDED);
        SET_ST(STAT_OFF);
      }
      continue;
    }

    SET_ST(STAT_RUNNING);
    CLR_ST(STAT_SUSPENDED);

    if (ST(STAT_OFF)) {
      LCD_power_on();
      rtc_wakeup_delay();
      CLR_ST(STAT_OFF);
      game_on_wake();
      next = sys_current_ms();
    }

    // Keys
    int k;
    while ((k = key_pop()) >= 0) {
      reset_auto_off();
      if (k > 0) handle_key(k);
      if (ST(STAT_PGM_END)) break;
    }
    if (ST(STAT_PGM_END)) continue;

    // One game tick
    int was_active = active;
    active = game_tick();
    if (!active) { plat_tone(0); continue; }

    reset_auto_off();  // don't auto power-off while playing
    if (!was_active) next = sys_current_ms();

    // Wait for the next tick while keeping the sound going
    next += TICK_MS;
    for (;;) {
      game_sfx_update();
      int32_t left = (int32_t)(next - sys_current_ms());
      if (left <= 0) break;
      if (!key_empty()) break;
      sys_delay(left > 4 ? 4 : left);
    }
    // keys break the wait early; keep the tick rate steady anyway
    while (!key_empty()) {
      int kk = key_pop();
      if (kk > 0) handle_key(kk);
      if (ST(STAT_PGM_END)) break;
    }
    for (;;) {
      game_sfx_update();
      int32_t left = (int32_t)(next - sys_current_ms());
      if (left <= 0) break;
      sys_delay(left > 4 ? 4 : left);
    }
    if ((int32_t)(sys_current_ms() - next) > 200) next = sys_current_ms();  // fell behind
  }
}
