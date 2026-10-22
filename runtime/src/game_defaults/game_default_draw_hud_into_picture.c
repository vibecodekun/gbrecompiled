/* Auto-split default for one game hook. One strong symbol per translation
 * unit: a game's extras.c that overrides only SOME hooks links cleanly —
 * the archive pulls only the per-hook objects the game does not provide,
 * so there is never a multiple-definition clash. mingw-safe (no reliance on
 * weak symbols in static archives). See game_extras.h. */
#include "game_extras.h"

void game_draw_hud_into_picture(struct GBContext *ctx, uint32_t *pixels, int width, int height, int native) {
    (void)ctx; (void)pixels; (void)width; (void)height; (void)native;
}
