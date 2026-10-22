/* Auto-split default for one game hook. One strong symbol per translation
 * unit: a game's extras.c that overrides only SOME hooks links cleanly —
 * the archive pulls only the per-hook objects the game does not provide,
 * so there is never a multiple-definition clash. mingw-safe (no reliance on
 * weak symbols in static archives). See game_extras.h. */
#include "game_extras.h"
#include <stddef.h>

const GBGameAction *game_get_actions(int *count) {
    if (count) *count = 0;
    return NULL;
}
