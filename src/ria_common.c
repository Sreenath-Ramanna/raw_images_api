/*
 * ria_common.c — version, status strings, format descriptions.
 */

#include "ria_internal.h"

#include <libraw/libraw.h>
#include <ctype.h>
#include <stdio.h>

static char g_version[32];

const char* ria_version_string(void) {
    if (g_version[0] == '\0') {
        snprintf(g_version, sizeof(g_version), "%d.%d.%d", RIA_VERSION_MAJOR,
                 RIA_VERSION_MINOR, RIA_VERSION_PATCH);
    }
    return g_version;
}

const char* ria_libraw_version_string(void) { return libraw_version(); }

const char* ria_status_string(ria_status status) {
    switch (status) {
        case RIA_OK:              return "ok";
        case RIA_ERR_INVALID:     return "invalid argument";
        case RIA_ERR_MEMORY:      return "out of memory";
        case RIA_ERR_IO:          return "file could not be read";
        case RIA_ERR_UNSUPPORTED: return "unsupported format or build option";
        case RIA_ERR_DECODE:      return "raw data could not be decoded";
        case RIA_ERR_NO_DATA:     return "the file has no such record";
        case RIA_ERR_INTERNAL:    return "internal error";
    }
    return "unknown status";
}

/* ── Pixel format description ────────────────────────────────────────────── */

int ria_format_channels(ria_pixel_format fmt) {
    switch (fmt) {
        case RIA_FMT_GRAY8:
        case RIA_FMT_GRAY16:  return 1;
        case RIA_FMT_RGB8:
        case RIA_FMT_RGB16:   return 3;
        case RIA_FMT_RGBA8:
        case RIA_FMT_RGBA16:  return 4;
    }
    return 0;
}

int ria_format_bits(ria_pixel_format fmt) {
    switch (fmt) {
        case RIA_FMT_RGB8:
        case RIA_FMT_RGBA8:
        case RIA_FMT_GRAY8:   return 8;
        case RIA_FMT_RGB16:
        case RIA_FMT_RGBA16:
        case RIA_FMT_GRAY16:  return 16;
    }
    return 0;
}

int ria_format_has_alpha(ria_pixel_format fmt) {
    return fmt == RIA_FMT_RGBA8 || fmt == RIA_FMT_RGBA16;
}

/* ── File extensions ─────────────────────────────────────────────────────── */

/* What LibRaw can open is far wider than this; the list is what a file
 * browser should offer, and covers every body this has been tested against.
 * Keep `.raw` last — it is a catch-all several vendors have used. */
static const char* const k_extensions[] = {
    ".cr2", ".cr3", ".crw",   /* Canon                                      */
    ".nef", ".nrw",           /* Nikon                                      */
    ".arw", ".srf", ".sr2",   /* Sony                                       */
    ".raf",                   /* Fujifilm                                   */
    ".orf",                   /* Olympus / OM System                        */
    ".pef", ".dng",           /* Pentax, Adobe                              */
    ".rw2",                   /* Panasonic                                  */
    ".rwl",                   /* Leica                                      */
    ".iiq",                   /* Phase One                                  */
    ".3fr", ".fff",           /* Hasselblad                                 */
    ".erf",                   /* Epson                                      */
    ".mrw",                   /* Minolta                                    */
    ".x3f",                   /* Sigma                                      */
    ".raw",
    NULL
};

const char* const* ria_supported_extensions(void) { return k_extensions; }

int ria_is_raw_extension(const char* path) {
    if (!path) return 0;

    const char* dot = strrchr(path, '.');
    if (!dot || dot[1] == '\0') return 0;

    /* Only the basename's extension counts: a directory called "photos.raw"
     * must not make every file inside it look like a RAW. */
    const char* slash = strrchr(path, '/');
    if (slash && dot < slash) return 0;

    char ext[16];
    size_t n = strlen(dot);
    if (n >= sizeof(ext)) return 0;
    for (size_t i = 0; i < n; i++) {
        ext[i] = (char)tolower((unsigned char)dot[i]);
    }
    ext[n] = '\0';

    for (int i = 0; k_extensions[i]; i++) {
        if (strcmp(ext, k_extensions[i]) == 0) return 1;
    }
    return 0;
}
