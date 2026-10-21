/* See rom_patch.h. */
#include "rom_patch.h"
#include "bps_patch.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

GBPatchFormat gb_patch_format(const uint8_t *patch, size_t patch_len) {
    if (!patch) return GB_PATCH_FORMAT_UNKNOWN;
    if (patch_len >= 5 && memcmp(patch, "IPS32", 5) == 0) return GB_PATCH_FORMAT_IPS32;
    if (patch_len >= 5 && memcmp(patch, "PATCH", 5) == 0) return GB_PATCH_FORMAT_IPS;
    if (patch_len >= 4 && memcmp(patch, "BPS1", 4) == 0) return GB_PATCH_FORMAT_BPS;
    return GB_PATCH_FORMAT_UNKNOWN;
}

static size_t read_be(const uint8_t *p, size_t width) {
    size_t value = 0;
    for (size_t i = 0; i < width; ++i) value = (value << 8) | p[i];
    return value;
}

/* Records of `offset_width`-byte offsets and 2-byte sizes (0 = a run of one
 * byte), up to `footer`; plain IPS may follow it with a 3-byte length the
 * output is cut to. */
static int ips_apply(const uint8_t *patch, size_t patch_len, size_t offset_width,
                     const char *footer, const uint8_t *src, size_t src_len,
                     uint8_t **out, size_t *out_len, char *err, size_t err_size) {
    const size_t footer_len = strlen(footer);
    size_t len = src_len, cap = src_len ? src_len : 1;
    uint8_t *o = (uint8_t *)malloc(cap);
    if (!o) { snprintf(err, err_size, "out of memory"); return 1; }
    if (src_len) memcpy(o, src, src_len);

    size_t pos = 5;
    for (;;) {
        if (pos + footer_len <= patch_len && memcmp(patch + pos, footer, footer_len) == 0) {
            pos += footer_len;
            break;
        }
        if (pos + offset_width + 2 > patch_len) {
            free(o);
            snprintf(err, err_size, "truncated patch (no %s marker)", footer);
            return 1;
        }
        const size_t offset = read_be(patch + pos, offset_width);
        size_t size = read_be(patch + pos + offset_width, 2);
        pos += offset_width + 2;
        int run = 0;
        uint8_t value = 0;
        if (size == 0) {
            if (pos + 3 > patch_len) { free(o); snprintf(err, err_size, "truncated run record"); return 1; }
            size = read_be(patch + pos, 2);
            value = patch[pos + 2];
            pos += 3;
            run = 1;
        } else if (pos + size > patch_len) {
            free(o);
            snprintf(err, err_size, "truncated record at offset %zu", offset);
            return 1;
        }
        const size_t end = offset + size;
        if (end > 0x10000000u) { free(o); snprintf(err, err_size, "record past 256 MB"); return 1; }
        if (end > cap) {
            size_t ncap = cap;
            while (ncap < end) ncap *= 2;
            uint8_t *grown = (uint8_t *)realloc(o, ncap);
            if (!grown) { free(o); snprintf(err, err_size, "out of memory"); return 1; }
            o = grown;
            cap = ncap;
        }
        if (offset > len) memset(o + len, 0, offset - len);
        if (run) {
            memset(o + offset, value, size);
        } else {
            memcpy(o + offset, patch + pos, size);
            pos += size;
        }
        if (end > len) len = end;
    }
    if (offset_width == 3 && patch_len - pos == 3) {
        const size_t truncate = read_be(patch + pos, 3);
        if (truncate < len) len = truncate;
    }
    *out = o;
    *out_len = len;
    return 0;
}

int gb_patch_apply(const uint8_t *patch, size_t patch_len,
                   const uint8_t *src, size_t src_len,
                   uint8_t **out, size_t *out_len,
                   char *err, size_t err_size) {
    if (!out || !out_len) return 1;
    *out = NULL;
    *out_len = 0;
    switch (gb_patch_format(patch, patch_len)) {
        case GB_PATCH_FORMAT_IPS:
            return ips_apply(patch, patch_len, 3, "EOF", src, src_len, out, out_len, err, err_size);
        case GB_PATCH_FORMAT_IPS32:
            return ips_apply(patch, patch_len, 4, "EEOF", src, src_len, out, out_len, err, err_size);
        case GB_PATCH_FORMAT_BPS:
            return gb_bps_apply(patch, patch_len, src, src_len, out, out_len, err, err_size);
        default:
            snprintf(err, err_size, "not an IPS or BPS patch");
            return 1;
    }
}
