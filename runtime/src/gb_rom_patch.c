/* See gb_rom_patch.h. */
#include "gb_rom_patch.h"
#include "gbrt.h"
#include "gb_host_paths.h"
#include "gb_sha256.h"
#include "launcher.h"
#include "rom_patch.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#elif defined(RECOMP_LAUNCHER)
#  include <SDL.h>
#endif

static const GBPatchTables *s_tables;
static char s_active[260];
static unsigned long long s_ran;    /* patched instructions run */
static size_t s_ran_positions;      /* distinct positions among them */
static uint8_t *s_ran_seen[512];

void gb_rom_patch_ran(unsigned window, unsigned slot) {
    s_ran++;
    if (window >= 512) return;
    if (!s_ran_seen[window]) s_ran_seen[window] = (uint8_t *)calloc(0x8000, 1);
    if (s_ran_seen[window] && !s_ran_seen[window][slot]) {
        s_ran_seen[window][slot] = 1;
        s_ran_positions++;
    }
}

void gb_rom_patch_counts(unsigned long long *ran, size_t *positions) {
    *ran = s_ran;
    *positions = s_ran_positions;
}

static void log_ran(void) {
    fprintf(stderr, "[ROM patch] %s: ran %llu patched instructions at %zu positions\n",
            s_active, s_ran, s_ran_positions);
}

void gb_rom_patch_register(const GBPatchTables *tables) {
    s_tables = tables;
    if (!tables) return;
    for (size_t i = 0; i < tables->known_count; ++i)
        launcher_add_accepted_sha256(tables->known[i].result_sha256);
}

const GBPatchTables *gb_rom_patch_tables(void) { return s_tables; }

const char *gb_rom_patch_active(void) { return s_active; }

uint8_t *gb_rom_patch_read_file(const char *path, size_t *size) {
    FILE *f = NULL;
#ifdef _WIN32
    /* The launcher's paths are UTF-8; fopen would read them as the ANSI page. */
    wchar_t wide[1024];
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, wide, 1024) > 0)
        f = _wfopen(wide, L"rb");
#endif
    if (!f) f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *data = n >= 0 ? (uint8_t *)malloc((size_t)n + 1) : NULL;
    if (data && n > 0 && fread(data, 1, (size_t)n, f) != (size_t)n) {
        free(data);
        data = NULL;
    }
    fclose(f);
    if (data) *size = (size_t)n;
    return data;
}

/* runtime_prefs.ini holds flat key=value lines. */
static int pref_value(const char *key, char *out, size_t out_size) {
    char path[1024];
    gb_host_state_path("runtime_prefs.ini", path, sizeof(path));
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    char line[1200];
    const size_t key_len = strlen(key);
    int found = 0;
    while (fgets(line, sizeof(line), f)) {
        char *s = line;
        while (*s == ' ' || *s == '\t') ++s;
        if (strncmp(s, key, key_len) != 0) continue;
        s += key_len;
        while (*s == ' ' || *s == '\t') ++s;
        if (*s != '=') continue;
        ++s;
        size_t n = strcspn(s, "\r\n");
        if (n >= out_size) n = out_size - 1;
        memcpy(out, s, n);
        out[n] = '\0';
        found = 1;
    }
    fclose(f);
    return found;
}

int gb_rom_patch_setting(char *path, size_t path_size) {
    char enabled[16];
    if (!pref_value("rom.patch_enabled", enabled, sizeof(enabled)) || atoi(enabled) == 0) return 0;
    return pref_value("rom.patch_file", path, path_size) && path[0];
}

static void rom_location(uint32_t offset, unsigned *bank, unsigned *address) {
    *bank = offset / 0x4000;
    *address = *bank ? 0x4000 + offset % 0x4000 : offset;
}

static int find_known(const GBPatchTables *t, const uint8_t *image, size_t size) {
    char digest[65];
    gb_sha256_hex(image, size, digest);
    for (size_t i = 0; i < t->known_count; ++i)
        if (strcmp(digest, t->known[i].result_sha256) == 0) return (int)i;
    return -1;
}

static const GBPatchSlot *find_slot(const GBPatchTables *t, uint32_t slot, const uint8_t *bytes, unsigned length) {
    size_t lo = 0, hi = t->slot_count;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (t->slots[mid].slot < slot) lo = mid + 1;
        else hi = mid;
    }
    for (; lo < t->slot_count && t->slots[lo].slot == slot; ++lo)
        if (t->slots[lo].length == length && memcmp(t->slots[lo].bytes, bytes, length) == 0)
            return &t->slots[lo];
    return NULL;
}

/* `changed` has a byte per ROM byte, nonzero where the patch changed it.
 * With `install`, the overrides and routine flags are set for this image. */
static void match(const GBPatchTables *t, const uint8_t *rom, size_t size, const uint8_t *changed,
                  GBPatchReport *r, int install) {
    uint8_t *code = (uint8_t *)calloc(size, 1);   /* bytes of analyzed instructions */
    for (size_t i = 0; i < t->body_range_count; ++i) {
        const GBPatchBodyRange *b = &t->body_ranges[i];
        if (b->offset + b->length <= size) memset(code + b->offset, 1, b->length);
    }
    if (install) {
        for (unsigned w = 0; w < t->windows; ++w) {
            free(t->overrides[w]);
            t->overrides[w] = NULL;
        }
        memset(t->patched_bodies, 0, t->body_count);
    }
    for (size_t i = 0; i < size; ++i) r->changed_bytes += changed[i] != 0;

    for (unsigned w = 0; w < t->windows; ++w) {
        const size_t base = (size_t)(w ? w - 1 : 0) * 0x4000;
        if (base + 0x4000 > size) continue;
        int any = 0;
        for (size_t i = 0; i < 0x4000 && !any; ++i) any = changed[base + i] != 0;
        if (!any) continue;
        for (unsigned bug = 0; bug < 2; ++bug) {
            for (unsigned off = 0; off < 0x4000; ++off) {
                /* The bytes the position's body was compiled from: its operands
                 * re-read the opcode after a HALT bug, and past the window's end
                 * they are read live. */
                unsigned n = t->opcode_lengths[rom[base + off]] - bug;
                if (n < 1) n = 1;
                if (off + n > 0x4000) n = 0x4000 - off;
                int dirty = 0;
                for (unsigned k = 0; k < n && !dirty; ++k) dirty = changed[base + off + k] != 0;
                if (!dirty) continue;
                r->slots++;
                const uint32_t slot = GB_PATCH_SLOT(w, bug, off);
                const GBPatchSlot *found = find_slot(t, slot, rom + base + off, n);
                if (found) {
                    r->compiled++;
                } else {
                    /* Changed bytes of analyzed code under it are counted below, once each. */
                    int in_code = 0;
                    for (unsigned k = 0; k < n; ++k)
                        if (changed[base + off + k] && code[base + off + k]) {
                            code[base + off + k] = 2;
                            in_code = 1;
                        }
                    if (!in_code) r->uncompiled_other++;
                }
                if (install) {
                    if (!t->overrides[w]) t->overrides[w] = (uint32_t *)calloc(0x8000, sizeof(uint32_t));
                    if (t->overrides[w])
                        t->overrides[w][bug * 0x4000 + off] = found ? found->id + 1 : 0xFFFFFFFFu;
                }
            }
        }
    }

    for (size_t i = 0; i < size; ++i)
        if (code[i] == 2) {
            if (!r->uncompiled_code) r->first_uncompiled = (uint32_t)i;
            r->uncompiled_code++;
        }

    uint32_t last = UINT32_MAX;
    for (size_t i = 0; i < t->body_range_count; ++i) {
        const GBPatchBodyRange *b = &t->body_ranges[i];
        if (b->body == last || b->offset + b->length > size) continue;
        for (uint32_t k = 0; k < b->length; ++k) {
            if (!changed[b->offset + k]) continue;
            last = b->body;
            r->bodies++;
            if (install) t->patched_bodies[b->body] = 1;
            break;
        }
    }
    free(code);
}

void gb_rom_patch_assess(const uint8_t *stock, size_t stock_size,
                         const uint8_t *patched, size_t patched_size,
                         GBPatchReport *report) {
    memset(report, 0, sizeof(*report));
    report->known = -1;
    report->source = -1;
    const GBPatchTables *t = s_tables;
    if (patched_size != stock_size || (t && patched_size != t->rom_size)) {
        report->size_changed = 1;
        return;
    }
    if (!t) return;
    char digest[65];
    gb_sha256_hex(stock, stock_size, digest);
    if (strcmp(digest, t->stock_sha256) != 0) {
        const int known = find_known(t, stock, stock_size);
        report->source = known >= 0 ? known : -2;
        return;
    }
    uint8_t *changed = (uint8_t *)malloc(stock_size);
    if (!changed) return;
    for (size_t i = 0; i < stock_size; ++i) changed[i] = stock[i] != patched[i];
    report->known = find_known(t, patched, patched_size);
    match(t, patched, patched_size, changed, report, 0);
    free(changed);
}

void gb_rom_patch_install(const uint8_t *stock, const uint8_t *patched, size_t size, GBPatchReport *report) {
    memset(report, 0, sizeof(*report));
    report->known = -1;
    report->source = -1;
    const GBPatchTables *t = s_tables;
    uint8_t *changed = t ? (uint8_t *)malloc(size) : NULL;
    if (!changed) return;
    for (size_t i = 0; i < size; ++i) changed[i] = stock[i] != patched[i];
    report->known = find_known(t, patched, size);
    match(t, patched, size, changed, report, 1);
    free(changed);
}

int gb_rom_patch_describe(const GBPatchReport *r, char *message, size_t size) {
    const GBPatchTables *t = s_tables;
    if (!t) {
        snprintf(message, size, "This version of the game can't check patches.");
        return 0;
    }
    if (r->source >= 0) {
        snprintf(message, size, "The ROM picked is already patched (%s). Patches apply to the original ROM: "
                 "pick that one, or turn the patch off.", t->known[r->source].title);
        return 0;
    }
    if (r->source == -2) {
        snprintf(message, size, "Patches apply to the original ROM: pick that one, or turn the patch off.");
        return 0;
    }
    if (r->size_changed) {
        snprintf(message, size, "This patch changes the ROM's size, which this version of the game can't run.");
        return 0;
    }
    if (!r->changed_bytes) {
        snprintf(message, size, "This patch changes nothing in this ROM.");
        return 1;
    }
    if (r->known >= 0) {
        snprintf(message, size, "Recognized: %s. It is built into this version of the game, so everything "
                 "it changes works.", t->known[r->known].title);
        return 1;
    }
    if (!r->uncompiled_code) {
        snprintf(message, size, "This patch changes the game's text, graphics or other data, not its code, "
                 "so it works.");
        return 1;
    }
    unsigned bank, address;
    rom_location(r->first_uncompiled, &bank, &address);
    snprintf(message, size, "This patch changes the game's code in %zu place%s this version wasn't built for "
             "(the first at %02X:%04X). The game stops with a message if it reaches one. Patches made for "
             "the game can be built in: see the README.",
             r->uncompiled_code, r->uncompiled_code == 1 ? "" : "s", bank, address);
    return 2;
}

static const char *path_leaf(const char *path) {
    const char *leaf = path;
    for (const char *p = path; *p; ++p)
        if (*p == '/' || *p == '\\') leaf = p + 1;
    return leaf;
}

static void log_report(const char *name, const GBPatchReport *r) {
    static int logging_ran;
    fprintf(stderr, "[ROM patch] %s: %zu bytes changed, %zu instruction positions, %zu compiled, "
            "%zu without native code in analyzed code, %zu elsewhere; %zu routines step through changes\n",
            name, r->changed_bytes, r->slots, r->compiled, r->uncompiled_code, r->uncompiled_other, r->bodies);
    if (!logging_ran) {
        logging_ran = 1;
        atexit(log_ran);
    }
}

int gb_rom_patch_boot(uint8_t **rom, size_t *size, char *err, size_t err_size) {
    s_active[0] = '\0';
    const GBPatchTables *t = s_tables;
    char path[1024];
    const int chosen = gb_rom_patch_setting(path, sizeof(path));
    if (!t) {
        if (chosen) {
            snprintf(err, err_size, "This build can't apply ROM patches.");
            return 0;
        }
        return 1;
    }
    if (*size != t->rom_size) return 1;
    GBPatchReport report;
    memset(&report, 0, sizeof(report));
    report.known = -1;

    char digest[65];
    gb_sha256_hex(*rom, *size, digest);
    if (strcmp(digest, t->stock_sha256) != 0) {
        /* A ROM already patched with a known patch. */
        const int known = find_known(t, *rom, *size);
        if (known < 0) return 1;
        const GBKnownPatch *k = &t->known[known];
        uint8_t *changed = (uint8_t *)calloc(*size, 1);
        if (!changed) { snprintf(err, err_size, "out of memory"); return 0; }
        for (uint32_t i = 0; i < k->changed_count; ++i)
            memset(changed + k->changed[i].offset, 1, k->changed[i].length);
        report.known = known;
        match(t, *rom, *size, changed, &report, 1);
        free(changed);
        snprintf(s_active, sizeof(s_active), "%s", k->title);
        log_report(s_active, &report);
        return 1;
    }
    if (!chosen) return 1;

    size_t patch_size = 0;
    uint8_t *patch = gb_rom_patch_read_file(path, &patch_size);
    if (!patch) {
        snprintf(err, err_size, "The ROM patch %s could not be read.", path);
        return 0;
    }
    uint8_t *patched = NULL;
    size_t patched_size = 0;
    char reason[192];
    const int rc = gb_patch_apply(patch, patch_size, *rom, *size, &patched, &patched_size, reason, sizeof(reason));
    free(patch);
    if (rc != 0) {
        snprintf(err, err_size, "The ROM patch %s does not apply to this ROM: %s.", path_leaf(path), reason);
        return 0;
    }
    if (patched_size != *size) {
        free(patched);
        snprintf(err, err_size, "The ROM patch %s changes the ROM's size, which this build can't run.",
                 path_leaf(path));
        return 0;
    }
    uint8_t *changed = (uint8_t *)malloc(*size);
    if (!changed) { free(patched); snprintf(err, err_size, "out of memory"); return 0; }
    for (size_t i = 0; i < *size; ++i) changed[i] = (*rom)[i] != patched[i];
    report.known = find_known(t, patched, patched_size);
    match(t, patched, patched_size, changed, &report, 1);
    free(changed);
    free(*rom);
    *rom = patched;
    snprintf(s_active, sizeof(s_active), "%s", report.known >= 0 ? t->known[report.known].title : path_leaf(path));
    log_report(s_active, &report);
    return 1;
}

static void show_message(const char *message) {
#ifdef _WIN32
    MessageBoxA(NULL, message, "ROM patch", MB_ICONWARNING | MB_OK);
#elif defined(RECOMP_LAUNCHER)
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_WARNING, "ROM patch", message, NULL);
#else
    (void)message;
#endif
}

void gb_rom_patch_fail(const char *message) {
    char text[700];
    snprintf(text, sizeof(text), "%s\n\nChoose another patch, or turn it off, on the launcher's Mods page.",
             message);
    fprintf(stderr, "[ROM patch] %s\n", message);
    show_message(text);
    exit(1);
}

void gb_rom_patch_uncompiled(GBContext *ctx) {
    char message[640];
    snprintf(message, sizeof(message),
             "The ROM patch (%s) changed the game's code at %02X:%04X, and this build has no native code "
             "compiled for that change, so the game has to stop here. Your in-game saves are kept.\n\n"
             "Patches made for this game can be compiled into it: see the README.",
             s_active[0] ? s_active : "unknown", ctx->pc < 0x4000 ? 0 : ctx->rom_bank, ctx->pc);
    fprintf(stderr, "[ROM patch] %s\n", message);
    gb_context_save_ram(ctx);
    show_message(message);
    exit(1);
}
