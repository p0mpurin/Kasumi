#pragma once

#include <stdbool.h>

#include "gfn_client.h"

/* Box art from GeForce NOW, shown in the library. The network worker
 * downloads a small server-resized JPEG while no game is running, decodes
 * it to a 96x128 RGB565 image and keeps a copy on the SD card; the UI thread
 * turns decoded images into textures between frames. */

#define GAME_ART_WIDTH 96
#define GAME_ART_HEIGHT 128

/* Starts the art thread (after http_global_init); exit stops it. */
void game_art_init(void);
void game_art_exit(void);
/* While a game starts or runs: no downloads (the stream has the Wi-Fi). */
void game_art_pause(bool paused);

/* UI thread: ask for a game's art (cheap; repeated calls are fine). */
void game_art_want(const GfnGame *game);
/* Any thread: download these games' art to the SD cache in the background
 * (after the library loads), so scrolling later reads it from the card. */
void game_art_prefetch(const GfnGame *games, unsigned count);
/* UI thread, outside a frame: upload images the worker has decoded. */
void game_art_pump(void);
/* UI thread: draw the art at (x, y); false if it is not ready yet. */
bool game_art_draw(const GfnGame *game, float x, float y, float scale, float alpha);
/* The same, fading from alpha_top to alpha_bottom, upside down if flip
 * (the shelf's reflection). */
bool game_art_draw_fade(const GfnGame *game, float x, float y, float scale, float alpha_top,
                        float alpha_bottom, bool flip);

/* Art thread: fetch and decode one wanted image. True if it did work. */
bool game_art_work(void);
