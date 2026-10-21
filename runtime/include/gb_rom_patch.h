/* The player's ROM patch (IPS, IPS32 or BPS) on a recompiled game.
 *
 * Every native instruction was compiled from the bytes of the stock ROM, so
 * a patch that changes code must not run the stock instruction. With
 * exhaustive_rom the recompiler also compiles each patch in its rom_patches
 * folder: every instruction position whose bytes a patch changes gets a
 * native body compiled from the patched bytes (GBPatchSlot), and every
 * analyzed routine records the ROM bytes its instructions came from
 * (GBPatchBodyRange). At boot the runtime applies the player's patch to the
 * stock ROM and matches the result: a changed position runs the body
 * compiled from exactly its new bytes, an analyzed routine that contains a
 * changed instruction steps through those per-position bodies instead of
 * running its own code, and a changed position with no body compiled for its
 * bytes stops the game with a message if it is ever executed. There is no
 * interpreter on this path.
 *
 * Data-only patches (text, graphics, palettes, levels) need nothing compiled:
 * the game reads them from the patched ROM like any other data. */
#ifndef GB_ROM_PATCH_H
#define GB_ROM_PATCH_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct GBContext;

/* An instruction position: the ROM window (0 = fixed bank 0, bank + 1 for the
 * switchable window), the HALT-bug variant and the offset in the window. */
#define GB_PATCH_SLOT(window, bug, offset) \
    (((uint32_t)(window) << 15) | ((uint32_t)(bug) << 14) | (uint32_t)(offset))

/* The exhaustive id a changed position with no compiled body resolves to. */
#define GB_PATCH_UNCOMPILED_ID 0xFFFFFFFEu

/* A native body compiled for the patched bytes at one position: the ROM bytes
 * from the position that it was compiled from (operands past the window are
 * read live, so at most to the window's end). */
typedef struct {
    uint32_t slot;
    uint32_t id;
    uint8_t length;
    uint8_t bytes[3];
} GBPatchSlot;

/* ROM bytes (file offsets) holding an analyzed routine's instructions. */
typedef struct {
    uint32_t body;
    uint32_t offset;
    uint32_t length;
} GBPatchBodyRange;

typedef struct {
    uint32_t offset;
    uint32_t length;
} GBPatchRange;

/* A patch from the rom_patches folder this build was compiled with. */
typedef struct {
    const char *name;            /* its path in the folder */
    const char *title;           /* what players are told it is */
    const char *patch_sha256;    /* of the patch file */
    const char *result_sha256;   /* of the stock ROM with it applied */
    const GBPatchRange *changed; /* the bytes it changes */
    uint32_t changed_count;
} GBKnownPatch;

/* What the recompiler emits for one recompiled ROM. */
typedef struct GBPatchTables {
    const char *stock_sha256;
    size_t rom_size;
    unsigned windows;                 /* ROM banks + 1 */
    const uint8_t *opcode_lengths;    /* the recompiler's decoder, by opcode */
    const GBPatchSlot *slots;         /* sorted by slot */
    size_t slot_count;
    const GBPatchBodyRange *body_ranges;
    size_t body_range_count;
    uint8_t *patched_bodies;          /* one flag per analyzed routine */
    size_t body_count;
    uint32_t **overrides;             /* per window: 32768 ids + 1, 0 = stock */
    const GBKnownPatch *known;
    size_t known_count;
} GBPatchTables;

/* What one patched image does on this build. */
typedef struct {
    int known;                    /* index into GBPatchTables.known, or -1 */
    int source;                   /* the ROM it was applied to: -1 the stock ROM, a
                                     known patch's result (its index), -2 another */
    int size_changed;             /* the image is not the stock ROM's size */
    size_t changed_bytes;
    size_t slots;                 /* instruction positions whose bytes changed */
    size_t compiled;              /* of those, with a body compiled for their bytes */
    size_t uncompiled_code;       /* changed bytes of analyzed code a position without one covers */
    size_t uncompiled_other;      /* positions without one elsewhere: data unless executed */
    uint32_t first_uncompiled;    /* ROM offset of the first of those bytes */
    size_t bodies;                /* analyzed routines that step through changes */
} GBPatchReport;

/* main(), before the launcher: the tables, and the known patches' results as
 * ROM identities the launcher accepts. */
void gb_rom_patch_register(const GBPatchTables *tables);
const GBPatchTables *gb_rom_patch_tables(void);

/* runtime_prefs.ini: rom.patch_file and rom.patch_enabled. Returns 1 and the
 * path when a patch is chosen and on. */
int gb_rom_patch_setting(char *path, size_t path_size);

/* Read a whole file (UTF-8 paths on Windows too). malloc'd; NULL on error. */
uint8_t *gb_rom_patch_read_file(const char *path, size_t *size);

/* Match `patched` (the stock ROM with a patch applied) against the tables,
 * changing nothing. */
void gb_rom_patch_assess(const uint8_t *stock, size_t stock_size,
                         const uint8_t *patched, size_t patched_size,
                         GBPatchReport *report);

/* Match `patched` against `stock` and set this build's tables for it, as the
 * boot does (checks run patched images this way). */
void gb_rom_patch_install(const uint8_t *stock, const uint8_t *patched, size_t size, GBPatchReport *report);

/* The launcher's line for a report. Returns 1 when the patch can be played,
 * 2 when it can with a caution, 0 when it cannot. */
int gb_rom_patch_describe(const GBPatchReport *report, char *message, size_t size);

/* <prefix>_init, after the launcher loaded `*rom`: the stock ROM gets the
 * player's patch (if one is on), a known patch's result is matched as it is.
 * Replaces *rom/*size with the patched image. Returns 0 with a reason in
 * `err` when the chosen patch cannot be used. */
int gb_rom_patch_boot(uint8_t **rom, size_t *size, char *err, size_t err_size);

/* Says why the chosen patch can't be used (a message box where there is one)
 * and ends the program. */
void gb_rom_patch_fail(const char *message);

/* The patch in use ("" for none). */
const char *gb_rom_patch_active(void);

/* Every patched instruction run (the generated exhaustive lookup); with a
 * patch in use, the totals are logged at exit. */
void gb_rom_patch_ran(unsigned window, unsigned slot);
void gb_rom_patch_counts(unsigned long long *ran, size_t *positions);

/* A changed instruction with no compiled body was reached: saves the battery
 * RAM, says where, and ends the game. */
void gb_rom_patch_uncompiled(struct GBContext *ctx);

#ifdef __cplusplus
}
#endif

#endif
