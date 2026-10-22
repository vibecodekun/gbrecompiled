/*
 * game_extras.h -- Game-specific hook interface for GB recomp projects
 *
 * Each recompiled game implements these hooks in its own extras.c.
 * The runtime calls them at well-defined points in the frame loop.
 *
 * Modeled after nesrecomp/runner/game_extras.h and psxrecomp/runner/game_extras.h.
 */
#pragma once
#include <stdint.h>
#include "debug_server.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Forward declaration */
struct GBContext;

/* ---- Lifecycle hooks ---- */

/* Called once at startup, after runtime init.
 * Good place to call gb_debug_server_init(). */
void game_on_init(struct GBContext *ctx);

/* Called once per frame, before game logic runs.
 * Good place to call gb_debug_server_poll() and
 * gb_debug_server_wait_if_paused(). */
void game_on_frame(struct GBContext *ctx);

/* Called after VBlank / frame completion.
 * Good place to call gb_debug_server_record_frame()
 * and gb_debug_server_check_watchpoints(). */
void game_post_frame(struct GBContext *ctx);

/* ---- UI hook ---- */

/* Called once per frame from inside the runtime's ImGui settings window
 * (after the built-in sections, inside the scroll area). The game may emit
 * its own ImGui controls here — e.g. a "Pokemon Options" CollapsingHeader.
 * Implementations linked into a C++ TU can call ImGui:: directly (the
 * runtime and game share the one ImGui context). Default: no-op.
 * Keeps the agnostic core free of any game-specific UI. */
void game_draw_overlay(struct GBContext *ctx);

/* ---- Game actions: buttons of the game's own ----
 *
 * Actions beyond the Game Boy's eight buttons (a transformation, a map), bound
 * to keys and controller buttons like the runtime's own: listed in the
 * settings window's Controls, saved in runtime_prefs.ini as
 * keyboard.game_<id>.<slot> / controller.game_<id>.<slot>, and held only while
 * no menu is open. A default that another action already has (a binding the
 * player made, or a runtime default it replaces) is left unbound. Default: no
 * actions. */
#define GB_GAME_ACTION_MAX 16
typedef struct GBGameAction {
    const char *id;       /* config name: letters, digits and '_' */
    const char *label;    /* as Controls lists it */
    int keys[2];          /* default SDL_Scancodes; 0 none */
    int button;           /* default SDL_GameControllerButton; -1 none */
} GBGameAction;
const GBGameAction *game_get_actions(int *count);
/* Whether action i of game_get_actions() is held. */
int gb_platform_game_action_held(int i);
/* A binding as the runtime keeps it: kind 0 none, 1 a key (code an
 * SDL_Scancode), 2 a controller button (SDL_GameControllerButton), 3 / 4 a
 * controller axis pushed positive / negative (SDL_GameControllerAxis). */
typedef struct GBActionBinding { int kind, code; } GBActionBinding;
/* Action i's two keyboard and two controller bindings. */
void gb_platform_game_action_bindings(int i, GBActionBinding keyboard[2], GBActionBinding controller[2]);
/* The same for a Game Boy button: 0 Right, 1 Left, 2 Up, 3 Down, 4 A, 5 B,
 * 6 Select, 7 Start. */
void gb_platform_joypad_bindings(int button, GBActionBinding keyboard[2], GBActionBinding controller[2]);
/* How the runtime names a binding ("Z", "West Face", "Left Shoulder"...),
 * written to out. */
void gb_platform_binding_label(GBActionBinding binding, char *out, int size);
/* The connected controller's face-button family: -1 none, 0 unknown, 1 Xbox,
 * 2 PlayStation, 3 Nintendo. And which device was used last: 0 the keyboard,
 * 1 a controller. */
int gb_platform_controller_family(void);
int gb_platform_last_input_device(void);
/* The Game Boy buttons held, as the debug server's input mask: bit 0 Right,
 * 1 Left, 2 Up, 3 Down, 4 A, 5 B, 6 Select, 7 Start; whatever is pressed,
 * menus open or not (for a game screen that reads them while the game is held). */
int gb_platform_joypad_held(void);

/* ---- HUD and game screens ----
 *
 * The presented picture, in window coordinates (ImGui's), for a HUD drawn
 * beside or over it. */
typedef struct GBHudFrame {
    float window_w, window_h;
    float x, y, w, h;      /* the game picture */
    float scale;           /* window pixels per game pixel across */
    int menu_open;         /* a runtime menu is up (it draws over the HUD) */
    int picture_w, picture_h;   /* the picture's own pixels */
    int native;            /* it is the Game Boy's own 160 x 144 (no game view, or a frame it declined) */
    /* The window can draw a HUD over the picture apart from it. 0 with a
     * shader preset that draws over the whole window (a bezel): the picture
     * is somewhere inside what the preset draws, so the game draws its HUD
     * into the picture instead (game_draw_hud_into_picture). */
    int layers;
} GBHudFrame;
/* Called every presented frame inside the runtime's ImGui frame, before its
 * menus; ImGui's background draw list is drawn over the picture and under the
 * menus. Not called without a window. Default: nothing. */
void game_draw_hud(struct GBContext *ctx, const GBHudFrame *frame);
/* A game screen (a map) is open: the game is held, as by Pause in Menu, and
 * frames keep being presented (with game_draw_hud) until it closes. */
void gb_platform_set_game_screen(int open);
/* A texture for the HUD: argb (0xAARRGGBB, width x height) uploaded to slot
 * 0-31, nearest filtered. Returns it as an ImTextureID, 0 without a window. */
uintptr_t gb_platform_hud_texture(int slot, const uint32_t *argb, int width, int height);
/* Slot's texture through the running shader preset, as the window draws a
 * game's layer: a chain of its own at the size it is drawn (width x height in
 * window coordinates), so a preset that works per pixel (an LCD grid,
 * scanlines) works per HUD pixel. Its transparency is the texture's own.
 * Returns it as an ImTextureID, or 0 with no preset running (or one that
 * failed): draw the plain texture then. Slots 0-3 each have a chain of their
 * own, so they can be drawn in the same frame; slots past 3 share the
 * fourth's, whose output the last call of a frame leaves. */
uintptr_t gb_platform_hud_shaded(int slot, float width, float height);
/* Each presented frame while frame.layers would be 0 (a preset that draws
 * over the whole window), before the preset runs: the game may draw its HUD
 * into the picture, `pixels` (width x height, 0xAARRGGBB; `native` as in
 * GBHudFrame). Not called without a window. Default: nothing. */
void game_draw_hud_into_picture(struct GBContext *ctx, uint32_t *pixels, int width, int height, int native);

/* Built-in Mods for the shared preboot launcher. The game owns the provider
 * and persists its selection beside the executable. Default: no Mods. */
struct RecompLauncherCModProvider;
const struct RecompLauncherCModProvider *game_get_mods(const char *exe_dir);

/* ---- Multi-body hooks (one executable, several recompiled ROM bodies) ----
 *
 * Default: no bodies, so the generated project boots its own single body and
 * nothing here has any effect. See gb_body.h for the full contract. */
struct GBBody;

/* Every recompiled body this executable contains. Return NULL / *out_count = 0
 * (the default) for an ordinary single-body build. */
const struct GBBody *const *game_get_bodies(int *out_count);

/* Index into game_get_bodies()'s array of the body to boot. Called once, after
 * the pre-boot launcher has run (so a Mods toggle can decide), before the
 * GBContext is created. Return -1 to keep the generated project's own body.
 * Default: 0. */
int game_select_body(const struct GBBody *const *bodies, int count);

/* ---- Debug hooks ---- */

/* Fill game-specific data into the frame record.
 * Write up to 16 bytes into r->game_data[]. */
void game_fill_frame_record(GBFrameRecord *r);

/* Handle a game-specific debug command.
 * Return 1 if handled, 0 to fall through to "unknown command". */
int game_handle_debug_cmd(const char *cmd, int id, const char *json);

/* ---- Dispatch hooks ---- */

/* Called when gb_dispatch() can't find a compiled function.
 * Return 1 if the game handled it (e.g., SRAM code), 0 to fall through. */
int game_dispatch_override(struct GBContext *ctx, uint16_t addr);

/* ---- Extended view (widescreen) capability ---- */

/* Maximum view width this game supports (opt-in widescreen). Default 160 =
 * not opted in; a game that supports the extended view returns up to
 * GB_WS_MAX_VIEW_WIDTH (256). See gb_widescreen.h / docs/WIDESCREEN.md. */
uint16_t game_max_view_width(void);

/* Called exactly once, only after a non-native view width is authorized.
 * The game installs its widescreen hooks here (sidecar arming, bound
 * overrides, pillarbox policy). Default: no-op. */
void game_extended_view_init(struct GBContext *ctx,
                             uint32_t extra_left, uint32_t extra_right);

/* Called once at the start of each armed frame's margin render. A game may
 * refresh presentation-only backing data and must fail closed if its scene or
 * validation contract is not satisfied. Part of the opt-in hook group. */
void game_extended_view_update(struct GBContext *ctx);

/* Optional margin BG source. Return 1 with tile/attr filled to replace the
 * wrapping hardware-map cell, 0 to use normal VRAM, or -1 for finite-map
 * void (black). Called only for margin pixels after the game has opened its
 * pillarbox gate. Part of the opt-in hook group. */
int game_extended_view_bg_tile(struct GBContext *ctx, int screen_x,
                               uint8_t scanline, uint8_t *tile,
                               uint8_t *attr);

/* ---- Info ---- */

/* Return the game name for the window title. */
const char *game_get_name(void);

/* Return the launcher console identity for this game: "gb" (original Game Boy /
 * DMG branding) or "gbc" (Game Boy Color branding). Consumed by the recomp-ui
 * pre-boot launcher (launcher_ui_seam.c) to pick the platform label, theme, and
 * controller/logo art. Default: "gbc" (the ecosystem is Game Boy Color; every
 * color-capable and colorizable cart uses it). DMG-only titles (Tetris, Super
 * Mario Land) override this to "gb" in their extras.c. */
const char *game_get_platform(void);

/* Default hardware mode for this cart before any saved per-game choice, as a
 * GBHardwareModePref value (gbrt.h). A saved menu choice still wins.
 * Default: GB_HARDWARE_MODE_AUTO (derive from the cart header). */
int game_default_hardware_mode(void);

/* Preemptive frames (platform_sdl.h) for this game before the player sets
 * any. Default: 0 (off). */
int game_default_preemptive_frames(void);

/* Handle a CLI argument. Return 1 if consumed, 0 to ignore. */
int game_handle_arg(const char *arg, const char *next_arg);

/* ---- ROM identity hooks (NES-style) ---- */

/* Return the game's expected ROM CRC32. Return 0 to skip CRC validation.
 * Used when there's a single canonical ROM version. */
uint32_t game_get_expected_crc32(void);

/* Return a static array of valid ROM CRC32s for multi-version games
 * (e.g. Pokemon Red + Blue share code; only data differs).
 * Set *out_count to the array length. Return NULL to defer to
 * game_get_expected_crc32(). */
const uint32_t *game_get_valid_crcs(int *out_count);

#ifdef __cplusplus
}
#endif
