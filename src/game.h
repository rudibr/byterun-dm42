// SPDX-License-Identifier: MIT
// Copyright (c) 2026 rudibr
// game.h - BYTE RUN game core interface
#ifndef GAME_H
#define GAME_H

#include <stdint.h>

#define TICK_MS 40   // 25 logic ticks per second

// Abstract game keys
enum {
  GK_NONE = 0,
  GK_UP, GK_LEFT, GK_DOWN, GK_RIGHT,
  GK_START,   // ENTER
  GK_SOUND,   // F1: toggle sound
  GK_PAUSE,   // EXIT
  GK_OTHER
};

void game_init(void);
void game_key(int gk);
int  game_tick(void);        // advance one tick; returns 1 while game needs ticking
void game_sfx_update(void);  // call often (every few ms) while ticking
void game_on_wake(void);     // after power-on: pause + full redraw
void game_pause(void);       // pause if playing (e.g. before system menu)
void game_redraw(void);      // full redraw of current screen
void game_save(void);        // persist settings/high score if changed

#endif
