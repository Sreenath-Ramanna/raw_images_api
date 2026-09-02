/*
 * ria_internal.h — shared between translation units, not installed.
 */

#ifndef RIA_INTERNAL_H
#define RIA_INTERNAL_H

#include "raw_images_api.h"

#include <math.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

/*
 * ria_image is part of the ABI and is mirrored field-for-field by FFI
 * consumers, so ownership bookkeeping cannot live in it. Instead the struct
 * is embedded in a private one and handed out by interior pointer; the
 * bookkeeping sits in front of it. No globals, no lookup, no lock.
 */
#define RIA_IMAGE_MAGIC 0x52494131u /* "RIA1" */

typedef struct {
    uint32_t  magic;
    int       owns_data;
    ria_image pub;
} ria_image_priv;

static inline ria_image_priv* ria_priv(const ria_image* img) {
    return (ria_image_priv*)((char*)img - offsetof(ria_image_priv, pub));
}

/* Allocate the struct, and the pixels unless `pixels` is supplied (wrapped,
 * not owned). `zero` clears freshly allocated pixels. */
ria_status ria_image_alloc(int width, int height, ria_pixel_format fmt,
                           uint8_t* pixels, int zero, ria_image** out);

/* Same geometry and format as `like`, uninitialised pixels. */
ria_status ria_image_like(const ria_image* like, ria_image** out);

/* Carry transfer function and colourspace from one image to another. Every
 * operation producing a new image must call this, or a scene-referred buffer
 * silently comes back labelled display-referred. */
void ria_image_copy_encoding(ria_image* dst, const ria_image* src);

/*
 * Hand the pixel buffer to the caller and drop ownership of it, so that
 * ria_image_free releases only the struct. Used by the legacy ABI shim,
 * whose callers free the buffer with plain free() — without this the shim
 * would have to copy a hundred megabytes to change who owns them.
 */
uint8_t* ria_image_release_data(ria_image* img);

/*
 * Row-parallel pixel loops. Every loop this is applied to writes to a
 * distinct row per iteration, so there is nothing to synchronise. Defined as
 * a macro rather than written inline because it has to work inside other
 * macros, and because it must vanish cleanly when the build has no OpenMP —
 * a bare `#pragma omp` there would trip -Wunknown-pragmas.
 */
#ifdef _OPENMP
#  define RIA_PARALLEL_ROWS _Pragma("omp parallel for schedule(static)")
#else
#  define RIA_PARALLEL_ROWS
#endif

static inline float ria_clampf(float v, float lo, float hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

static inline int ria_clampi(int v, int lo, int hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

/* The photographic grey card, as a linear reflectance. Both the anchor of the
 * display transform and the default pivot for contrast, so it is defined once
 * — the two must agree or a neutral edit shifts brightness. */
#define RIA_MIDDLE_GREY 0.18

/* Rec.709 luma, defined once: greyscale conversion, saturation and the luma
 * histogram must agree or a round trip through them shifts brightness. */
#define RIA_LUMA_R 0.2126f
#define RIA_LUMA_G 0.7152f
#define RIA_LUMA_B 0.0722f

static inline float ria_luma(float r, float g, float b) {
    return RIA_LUMA_R * r + RIA_LUMA_G * g + RIA_LUMA_B * b;
}

/* Largest sample value at a given depth. */
static inline int ria_max_value(int bits) { return bits == 16 ? 65535 : 255; }

/* Channels excluding alpha. */
static inline int ria_color_channels(const ria_image* img) {
    return ria_format_has_alpha(img->format) ? img->channels - 1
                                             : img->channels;
}

static inline int ria_image_valid(const ria_image* img) {
    return img && img->data && img->width > 0 && img->height > 0;
}

#endif /* RIA_INTERNAL_H */
