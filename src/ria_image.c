/*
 * ria_image.c — pixel buffers, format conversion, geometry, PNM output.
 *
 * Every loop here runs over tens of millions of pixels on a modern frame, so
 * the shape is always the same: hoist everything invariant out, walk rows
 * with a plain pointer, and let OpenMP split the rows when it is available.
 */

#include "ria_internal.h"

#include <stdint.h>
#include <stdio.h>

/* ── Allocation ──────────────────────────────────────────────────────────── */

ria_status ria_image_alloc(int width, int height, ria_pixel_format fmt,
                           uint8_t* pixels, int zero, ria_image** out) {
    if (!out) return RIA_ERR_INVALID;
    *out = NULL;

    const int channels = ria_format_channels(fmt);
    const int bits = ria_format_bits(fmt);
    if (width <= 0 || height <= 0 || channels == 0) return RIA_ERR_INVALID;

    /* Guard the multiplication rather than trusting it: a corrupt header can
     * offer dimensions whose product overflows, and the result would be a
     * buffer far smaller than every loop below assumes. */
    const size_t bytes_per_pixel = (size_t)channels * (size_t)(bits / 8);
    if ((size_t)width > SIZE_MAX / bytes_per_pixel) return RIA_ERR_INVALID;
    const size_t stride = (size_t)width * bytes_per_pixel;
    if ((size_t)height > SIZE_MAX / stride) return RIA_ERR_INVALID;
    const size_t size = stride * (size_t)height;

    ria_image_priv* priv = (ria_image_priv*)calloc(1, sizeof(*priv));
    if (!priv) return RIA_ERR_MEMORY;

    if (pixels) {
        priv->owns_data = 0;
        priv->pub.data = pixels;
    } else {
        priv->owns_data = 1;
        priv->pub.data = (uint8_t*)(zero ? calloc(1, size) : malloc(size));
        if (!priv->pub.data) {
            free(priv);
            return RIA_ERR_MEMORY;
        }
    }

    priv->magic = RIA_IMAGE_MAGIC;
    priv->pub.width = width;
    priv->pub.height = height;
    priv->pub.channels = channels;
    priv->pub.bits = bits;
    priv->pub.stride = stride;
    priv->pub.data_size = size;
    priv->pub.format = fmt;
    priv->pub.pending_flip = RIA_FLIP_NONE;

    /* Assume display-referred sRGB for a buffer of unknown provenance — a
     * caller wrapping a screenshot or a decoded JPEG has that, and it is the
     * safe assumption because the operations that care refuse anything but
     * linear rather than trusting the label. ria_raw_decode overwrites this
     * with what it actually produced. */
    priv->pub.transfer = RIA_TRANSFER_SRGB;
    priv->pub.transfer_gamma = 2.4f;
    priv->pub.transfer_slope = 12.92f;
    priv->pub.colorspace = RIA_COLORSPACE_SRGB;

    /* 1.0 is "these samples were not renormalised", which is true of every
     * buffer except one a highlight-reconstructing decode rescaled. */
    priv->pub.saturation_level = 1.0f;

    *out = &priv->pub;
    return RIA_OK;
}

/* Geometry and format operations must carry the encoding across, or a
 * resized scene-referred image silently claims to be display-referred and the
 * next EV operation refuses it. */
void ria_image_copy_encoding(ria_image* dst, const ria_image* src) {
    dst->transfer = src->transfer;
    dst->transfer_gamma = src->transfer_gamma;
    dst->transfer_slope = src->transfer_slope;
    dst->colorspace = src->colorspace;
    dst->saturation_level = src->saturation_level;
}

ria_status ria_image_like(const ria_image* like, ria_image** out) {
    ria_status rc = ria_image_alloc(like->width, like->height, like->format,
                                    NULL, 0, out);
    if (rc == RIA_OK) {
        (*out)->pending_flip = like->pending_flip;
        ria_image_copy_encoding(*out, like);
    }
    return rc;
}

ria_status ria_image_new(int width, int height, ria_pixel_format fmt,
                         ria_image** out) {
    return ria_image_alloc(width, height, fmt, NULL, 0, out);
}

ria_status ria_image_new_zeroed(int width, int height, ria_pixel_format fmt,
                                ria_image** out) {
    return ria_image_alloc(width, height, fmt, NULL, 1, out);
}

ria_status ria_image_wrap(uint8_t* data, int width, int height,
                          ria_pixel_format fmt, ria_image** out) {
    if (!data) return RIA_ERR_INVALID;
    return ria_image_alloc(width, height, fmt, data, 0, out);
}

void ria_image_free(ria_image* img) {
    if (!img) return;
    ria_image_priv* priv = ria_priv(img);
    /* A wrong pointer here is a caller bug that would otherwise free an
     * arbitrary address. Leaking is the kinder failure. */
    if (priv->magic != RIA_IMAGE_MAGIC) return;
    priv->magic = 0;
    if (priv->owns_data) free(priv->pub.data);
    free(priv);
}

uint8_t* ria_image_release_data(ria_image* img) {
    if (!img) return NULL;
    ria_image_priv* priv = ria_priv(img);
    if (priv->magic != RIA_IMAGE_MAGIC) return NULL;
    priv->owns_data = 0;
    return img->data;
}

ria_status ria_image_clone(const ria_image* src, ria_image** out) {
    if (!ria_image_valid(src) || !out) return RIA_ERR_INVALID;
    ria_status rc = ria_image_like(src, out);
    if (rc != RIA_OK) return rc;
    memcpy((*out)->data, src->data, src->data_size);
    return RIA_OK;
}

/* ── Sample access ───────────────────────────────────────────────────────── */

/* Everything internal works in 16-bit sample space: 8-bit input is scaled by
 * 257 so that 255 maps exactly to 65535, and scaled back with rounding. Doing
 * it this way means one code path per operation instead of two. */

static inline uint16_t load8(const uint8_t* p) { return (uint16_t)(*p * 257); }

static inline uint8_t store8(uint16_t v) { return (uint8_t)((v + 128) / 257); }

/* Reads one pixel as RGBA in 16-bit space, whatever the source format is. */
static inline void read_rgba16(const uint8_t* p, ria_pixel_format fmt,
                               int channels, int bits, uint16_t rgba[4]) {
    if (bits == 8) {
        if (channels == 1) {
            rgba[0] = rgba[1] = rgba[2] = load8(p);
        } else {
            rgba[0] = load8(p);
            rgba[1] = load8(p + 1);
            rgba[2] = load8(p + 2);
        }
        rgba[3] = (channels == 4) ? load8(p + 3) : 65535;
    } else {
        const uint16_t* s = (const uint16_t*)p;
        if (channels == 1) {
            rgba[0] = rgba[1] = rgba[2] = s[0];
        } else {
            rgba[0] = s[0];
            rgba[1] = s[1];
            rgba[2] = s[2];
        }
        rgba[3] = (channels == 4) ? s[3] : 65535;
    }
    (void)fmt;
}

static inline void write_rgba16(uint8_t* p, int channels, int bits,
                                const uint16_t rgba[4]) {
    if (bits == 8) {
        if (channels == 1) {
            const float y = ria_luma(rgba[0], rgba[1], rgba[2]);
            p[0] = store8((uint16_t)(y + 0.5f));
        } else {
            p[0] = store8(rgba[0]);
            p[1] = store8(rgba[1]);
            p[2] = store8(rgba[2]);
            if (channels == 4) p[3] = store8(rgba[3]);
        }
    } else {
        uint16_t* d = (uint16_t*)p;
        if (channels == 1) {
            const float y = ria_luma(rgba[0], rgba[1], rgba[2]);
            d[0] = (uint16_t)(y + 0.5f);
        } else {
            d[0] = rgba[0];
            d[1] = rgba[1];
            d[2] = rgba[2];
            if (channels == 4) d[3] = rgba[3];
        }
    }
}

/* ── RGB -> RGBA ─────────────────────────────────────────────────────────── */

/*
 * One 32-bit store per pixel rather than four byte stores. Written as a
 * memcpy into a uint32 so that no alignment assumption is made about `dst`
 * — the compiler turns the pair into a single unaligned store, and -O3
 * vectorises the loop.
 *
 * The shift order produces R,G,B,A in ascending memory on a little-endian
 * host, which is what every "RGBA8888" consumer means by it.
 */
void ria_expand_rgb_to_rgba(const uint8_t* src, uint8_t* dst, size_t pixels) {
    if (!src || !dst) return;
    for (size_t i = 0; i < pixels; i++) {
        const uint8_t* p = src + i * 3;
        const uint32_t v = 0xFF000000u | ((uint32_t)p[2] << 16) |
                           ((uint32_t)p[1] << 8) | (uint32_t)p[0];
        memcpy(dst + i * 4, &v, sizeof(v));
    }
}

/* ── Format conversion ───────────────────────────────────────────────────── */

ria_status ria_image_convert(const ria_image* src, ria_pixel_format fmt,
                             ria_image** out) {
    if (!ria_image_valid(src) || !out) return RIA_ERR_INVALID;
    if (ria_format_channels(fmt) == 0) return RIA_ERR_INVALID;

    if (fmt == src->format) return ria_image_clone(src, out);

    ria_status rc = ria_image_alloc(src->width, src->height, fmt, NULL, 0, out);
    if (rc != RIA_OK) return rc;
    (*out)->pending_flip = src->pending_flip;
    ria_image_copy_encoding(*out, src);

    ria_image* dst = *out;
    const int sc = src->channels, sb = src->bits;
    const int dc = dst->channels, db = dst->bits;

    /* The one conversion that is worth a special case — it is what every
     * display path asks for after a decode. */
    if (src->format == RIA_FMT_RGB8 && fmt == RIA_FMT_RGBA8) {
        RIA_PARALLEL_ROWS
        for (int y = 0; y < src->height; y++) {
            ria_expand_rgb_to_rgba(src->data + (size_t)y * src->stride,
                                   dst->data + (size_t)y * dst->stride,
                                   (size_t)src->width);
        }
        return RIA_OK;
    }

    RIA_PARALLEL_ROWS
    for (int y = 0; y < src->height; y++) {
        const uint8_t* sp = src->data + (size_t)y * src->stride;
        uint8_t* dp = dst->data + (size_t)y * dst->stride;
        const size_t sbpp = (size_t)sc * (size_t)(sb / 8);
        const size_t dbpp = (size_t)dc * (size_t)(db / 8);
        for (int x = 0; x < src->width; x++) {
            uint16_t rgba[4];
            read_rgba16(sp + (size_t)x * sbpp, src->format, sc, sb, rgba);
            write_rgba16(dp + (size_t)x * dbpp, dc, db, rgba);
        }
    }
    return RIA_OK;
}

/* ── Crop ────────────────────────────────────────────────────────────────── */

ria_status ria_crop(const ria_image* src, int x, int y, int width, int height,
                    ria_image** out) {
    if (!ria_image_valid(src) || !out) return RIA_ERR_INVALID;
    if (width <= 0 || height <= 0 || x < 0 || y < 0) return RIA_ERR_INVALID;
    if (x + width > src->width || y + height > src->height) {
        return RIA_ERR_INVALID;
    }

    ria_status rc = ria_image_alloc(width, height, src->format, NULL, 0, out);
    if (rc != RIA_OK) return rc;
    (*out)->pending_flip = src->pending_flip;
    ria_image_copy_encoding(*out, src);

    const size_t bpp = (size_t)src->channels * (size_t)(src->bits / 8);
    for (int row = 0; row < height; row++) {
        memcpy((*out)->data + (size_t)row * (*out)->stride,
               src->data + (size_t)(y + row) * src->stride + (size_t)x * bpp,
               (*out)->stride);
    }
    return RIA_OK;
}

/* ── Orientation ─────────────────────────────────────────────────────────── */

/*
 * LibRaw flip codes: 0 upright, 3 = 180 degrees, 5 = 90 CCW, 6 = 90 CW.
 * Anything else is copied unchanged — a guess here silently produces a
 * sideways image, which is worse than doing nothing.
 *
 * The destination is written sequentially and the source is strided, rather
 * than the other way round, because a sequential write stream is friendlier
 * to the store buffer than a sequential read is to the prefetcher.
 */
ria_status ria_apply_orientation(const ria_image* src, int flip,
                                 ria_image** out) {
    if (!ria_image_valid(src) || !out) return RIA_ERR_INVALID;

    if (flip != RIA_FLIP_180 && flip != RIA_FLIP_90_CCW &&
        flip != RIA_FLIP_90_CW) {
        ria_status rc = ria_image_clone(src, out);
        if (rc == RIA_OK) (*out)->pending_flip = RIA_FLIP_NONE;
        return rc;
    }

    const int quarter = (flip == RIA_FLIP_90_CCW || flip == RIA_FLIP_90_CW);
    const int dw = quarter ? src->height : src->width;
    const int dh = quarter ? src->width : src->height;

    ria_status rc = ria_image_alloc(dw, dh, src->format, NULL, 0, out);
    if (rc != RIA_OK) return rc;
    (*out)->pending_flip = RIA_FLIP_NONE;
    ria_image_copy_encoding(*out, src);

    ria_image* dst = *out;
    const size_t bpp = (size_t)src->channels * (size_t)(src->bits / 8);

    RIA_PARALLEL_ROWS
    for (int dy = 0; dy < dh; dy++) {
        uint8_t* dp = dst->data + (size_t)dy * dst->stride;
        for (int dx = 0; dx < dw; dx++) {
            int sx, sy;
            switch (flip) {
                case RIA_FLIP_180:
                    sx = src->width - 1 - dx;
                    sy = src->height - 1 - dy;
                    break;
                case RIA_FLIP_90_CCW: /* top-right of the source becomes top-left */
                    sx = src->width - 1 - dy;
                    sy = dx;
                    break;
                default: /* RIA_FLIP_90_CW */
                    sx = dy;
                    sy = src->height - 1 - dx;
                    break;
            }
            memcpy(dp + (size_t)dx * bpp,
                   src->data + (size_t)sy * src->stride + (size_t)sx * bpp,
                   bpp);
        }
    }
    return RIA_OK;
}

/* ── Resize ──────────────────────────────────────────────────────────────── */

/*
 * Separable triangle filter with a support that widens as the image shrinks.
 * Enlarging (ratio <= 1) gives plain bilinear; reducing gives an area
 * average across every source pixel that lands in the destination footprint.
 *
 * The distinction matters: a bilinear downscale of a 33 MP frame to a
 * 400 px thumbnail reads four source pixels out of every ~7000 and turns
 * fine detail into noise. The cost of doing it properly is one pass over the
 * source, which is unavoidable anyway.
 */

typedef struct {
    int    start;   /* first source index                                    */
    int    count;   /* how many weights                                      */
    float* weights; /* into the shared pool                                  */
} ria_contrib;

typedef struct {
    ria_contrib* items;
    float*       pool;
} ria_contrib_list;

static void contribs_free(ria_contrib_list* list) {
    free(list->items);
    free(list->pool);
    list->items = NULL;
    list->pool = NULL;
}

static ria_status contribs_build(int src_n, int dst_n, ria_contrib_list* list) {
    const float ratio = (float)src_n / (float)dst_n;
    const float support = ratio > 1.0f ? ratio : 1.0f;
    const int max_taps = (int)(2.0f * support) + 3;

    list->items = (ria_contrib*)calloc((size_t)dst_n, sizeof(ria_contrib));
    list->pool = (float*)calloc((size_t)dst_n * (size_t)max_taps, sizeof(float));
    if (!list->items || !list->pool) {
        contribs_free(list);
        return RIA_ERR_MEMORY;
    }

    for (int i = 0; i < dst_n; i++) {
        const float center = ((float)i + 0.5f) * ratio - 0.5f;
        int left = (int)floorf(center - support + 0.5f);
        int right = (int)ceilf(center + support - 0.5f);
        if (right < left) right = left;

        float* w = list->pool + (size_t)i * (size_t)max_taps;
        list->items[i].weights = w;
        list->items[i].start = left;
        list->items[i].count = right - left + 1;
        if (list->items[i].count > max_taps) list->items[i].count = max_taps;

        float total = 0.0f;
        for (int k = 0; k < list->items[i].count; k++) {
            const float d = fabsf(((float)(left + k) - center) / support);
            const float weight = d < 1.0f ? 1.0f - d : 0.0f;
            w[k] = weight;
            total += weight;
        }
        /* A degenerate window (possible only through rounding at extreme
         * ratios) would otherwise divide by zero and blank the row. */
        if (total <= 0.0f) {
            for (int k = 0; k < list->items[i].count; k++) w[k] = 0.0f;
            w[0] = 1.0f;
        } else {
            for (int k = 0; k < list->items[i].count; k++) w[k] /= total;
        }
    }
    return RIA_OK;
}

/* Intermediate rows are held in 16-bit sample space whatever the source
 * depth, so an 8-bit resize does not round twice. */
static ria_status resize_triangle(const ria_image* src, int dw, int dh,
                                  ria_image* dst) {
    const int ch = src->channels;
    ria_contrib_list cx = {NULL, NULL}, cy = {NULL, NULL};
    ria_status rc = contribs_build(src->width, dw, &cx);
    if (rc != RIA_OK) return rc;
    rc = contribs_build(src->height, dh, &cy);
    if (rc != RIA_OK) {
        contribs_free(&cx);
        return rc;
    }

    /* Horizontal pass: src (w x h) -> tmp (dw x h), always uint16. */
    uint16_t* tmp = NULL;
    if ((size_t)dw <= SIZE_MAX / ((size_t)src->height * (size_t)ch * 2)) {
        tmp = (uint16_t*)malloc((size_t)dw * (size_t)src->height *
                                (size_t)ch * sizeof(uint16_t));
    }
    if (!tmp) {
        contribs_free(&cx);
        contribs_free(&cy);
        return RIA_ERR_MEMORY;
    }

    const int sb = src->bits;
    const int sw = src->width;

    RIA_PARALLEL_ROWS
    for (int y = 0; y < src->height; y++) {
        const uint8_t* srow = src->data + (size_t)y * src->stride;
        uint16_t* trow = tmp + (size_t)y * (size_t)dw * (size_t)ch;
        for (int x = 0; x < dw; x++) {
            const ria_contrib* c = &cx.items[x];
            float acc[4] = {0.0f, 0.0f, 0.0f, 0.0f};
            for (int k = 0; k < c->count; k++) {
                const int sx = ria_clampi(c->start + k, 0, sw - 1);
                const float w = c->weights[k];
                if (sb == 8) {
                    const uint8_t* p = srow + (size_t)sx * (size_t)ch;
                    for (int i = 0; i < ch; i++) acc[i] += w * (float)p[i];
                } else {
                    const uint16_t* p =
                        (const uint16_t*)srow + (size_t)sx * (size_t)ch;
                    for (int i = 0; i < ch; i++) acc[i] += w * (float)p[i];
                }
            }
            uint16_t* t = trow + (size_t)x * (size_t)ch;
            const float scale = (sb == 8) ? 257.0f : 1.0f;
            for (int i = 0; i < ch; i++) {
                t[i] = (uint16_t)ria_clampf(acc[i] * scale + 0.5f, 0.0f, 65535.0f);
            }
        }
    }

    /* Vertical pass: tmp (dw x h) -> dst (dw x dh), back to the source depth. */
    RIA_PARALLEL_ROWS
    for (int y = 0; y < dh; y++) {
        const ria_contrib* c = &cy.items[y];
        uint8_t* drow = dst->data + (size_t)y * dst->stride;
        for (int x = 0; x < dw; x++) {
            float acc[4] = {0.0f, 0.0f, 0.0f, 0.0f};
            for (int k = 0; k < c->count; k++) {
                const int sy = ria_clampi(c->start + k, 0, src->height - 1);
                const uint16_t* p = tmp + ((size_t)sy * (size_t)dw + (size_t)x) *
                                              (size_t)ch;
                const float w = c->weights[k];
                for (int i = 0; i < ch; i++) acc[i] += w * (float)p[i];
            }
            if (sb == 8) {
                uint8_t* d = drow + (size_t)x * (size_t)ch;
                for (int i = 0; i < ch; i++) {
                    d[i] = store8((uint16_t)ria_clampf(acc[i] + 0.5f, 0.0f,
                                                       65535.0f));
                }
            } else {
                uint16_t* d = (uint16_t*)drow + (size_t)x * (size_t)ch;
                for (int i = 0; i < ch; i++) {
                    d[i] = (uint16_t)ria_clampf(acc[i] + 0.5f, 0.0f, 65535.0f);
                }
            }
        }
    }

    free(tmp);
    contribs_free(&cx);
    contribs_free(&cy);
    return RIA_OK;
}

static void resize_nearest(const ria_image* src, int dw, int dh,
                           ria_image* dst) {
    const size_t bpp = (size_t)src->channels * (size_t)(src->bits / 8);
    RIA_PARALLEL_ROWS
    for (int y = 0; y < dh; y++) {
        const int sy = ria_clampi((int)(((float)y + 0.5f) * src->height / dh),
                                  0, src->height - 1);
        const uint8_t* srow = src->data + (size_t)sy * src->stride;
        uint8_t* drow = dst->data + (size_t)y * dst->stride;
        for (int x = 0; x < dw; x++) {
            const int sx = ria_clampi((int)(((float)x + 0.5f) * src->width / dw),
                                      0, src->width - 1);
            memcpy(drow + (size_t)x * bpp, srow + (size_t)sx * bpp, bpp);
        }
    }
}

ria_status ria_resize(const ria_image* src, int width, int height,
                      ria_resize_filter filter, ria_image** out) {
    if (!ria_image_valid(src) || !out) return RIA_ERR_INVALID;
    if (width <= 0 || height <= 0) return RIA_ERR_INVALID;

    if (width == src->width && height == src->height) {
        return ria_image_clone(src, out);
    }

    ria_status rc = ria_image_alloc(width, height, src->format, NULL, 0, out);
    if (rc != RIA_OK) return rc;
    (*out)->pending_flip = src->pending_flip;
    ria_image_copy_encoding(*out, src);

    if (filter == RIA_RESIZE_NEAREST) {
        resize_nearest(src, width, height, *out);
        return RIA_OK;
    }

    rc = resize_triangle(src, width, height, *out);
    if (rc != RIA_OK) {
        ria_image_free(*out);
        *out = NULL;
    }
    return rc;
}

ria_status ria_fit_within(const ria_image* src, int max_width, int max_height,
                          ria_resize_filter filter, ria_image** out) {
    if (!ria_image_valid(src) || !out) return RIA_ERR_INVALID;
    if (max_width <= 0 || max_height <= 0) return RIA_ERR_INVALID;

    if (src->width <= max_width && src->height <= max_height) {
        return ria_image_clone(src, out);
    }

    const double sx = (double)max_width / (double)src->width;
    const double sy = (double)max_height / (double)src->height;
    const double s = sx < sy ? sx : sy;

    int w = (int)(src->width * s + 0.5);
    int h = (int)(src->height * s + 0.5);
    if (w < 1) w = 1;
    if (h < 1) h = 1;
    /* Rounding can push one axis a pixel over the box. */
    if (w > max_width) w = max_width;
    if (h > max_height) h = max_height;

    return ria_resize(src, w, h, filter, out);
}

/* ── PNM output ──────────────────────────────────────────────────────────── */

ria_status ria_write_pnm(const ria_image* img, const char* path) {
    if (!ria_image_valid(img) || !path) return RIA_ERR_INVALID;

    const int color = img->channels >= 3;
    const int maxval = ria_max_value(img->bits);

    FILE* f = fopen(path, "wb");
    if (!f) return RIA_ERR_IO;

    if (fprintf(f, "P%d\n%d %d\n%d\n", color ? 6 : 5, img->width, img->height,
                maxval) < 0) {
        fclose(f);
        return RIA_ERR_IO;
    }

    const int out_ch = color ? 3 : 1;
    const size_t row_bytes = (size_t)img->width * (size_t)out_ch *
                             (size_t)(img->bits / 8);
    uint8_t* row = (uint8_t*)malloc(row_bytes);
    if (!row) {
        fclose(f);
        return RIA_ERR_MEMORY;
    }

    ria_status rc = RIA_OK;
    for (int y = 0; y < img->height && rc == RIA_OK; y++) {
        const uint8_t* sp = img->data + (size_t)y * img->stride;
        if (img->bits == 8) {
            for (int x = 0; x < img->width; x++) {
                for (int i = 0; i < out_ch; i++) {
                    row[x * out_ch + i] = sp[x * img->channels + i];
                }
            }
        } else {
            /* PNM stores 16-bit samples big-endian. */
            const uint16_t* s = (const uint16_t*)sp;
            for (int x = 0; x < img->width; x++) {
                for (int i = 0; i < out_ch; i++) {
                    const uint16_t v = s[x * img->channels + i];
                    row[(x * out_ch + i) * 2] = (uint8_t)(v >> 8);
                    row[(x * out_ch + i) * 2 + 1] = (uint8_t)(v & 0xFF);
                }
            }
        }
        if (fwrite(row, 1, row_bytes, f) != row_bytes) rc = RIA_ERR_IO;
    }

    free(row);
    if (fclose(f) != 0 && rc == RIA_OK) rc = RIA_ERR_IO;
    return rc;
}
