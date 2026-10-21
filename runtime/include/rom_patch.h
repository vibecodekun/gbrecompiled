/* ROM patch formats: IPS (with the truncation extension), IPS32 and BPS.
 * Shared by the recompiler, which compiles the code a known patch changes,
 * and the runtime, which applies the player's patch at boot, so both derive
 * the same image. */
#ifndef GB_ROM_PATCH_FORMAT_H
#define GB_ROM_PATCH_FORMAT_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    GB_PATCH_FORMAT_UNKNOWN = 0,
    GB_PATCH_FORMAT_IPS,
    GB_PATCH_FORMAT_IPS32,
    GB_PATCH_FORMAT_BPS,
} GBPatchFormat;

GBPatchFormat gb_patch_format(const uint8_t *patch, size_t patch_len);

/* Apply `patch` to `src`. On success allocates *out (malloc, caller frees)
 * and returns 0; otherwise returns non-zero with a reason in `err`. BPS
 * checks the source, target and patch checksums. */
int gb_patch_apply(const uint8_t *patch, size_t patch_len,
                   const uint8_t *src, size_t src_len,
                   uint8_t **out, size_t *out_len,
                   char *err, size_t err_size);

#ifdef __cplusplus
}
#endif

#endif
