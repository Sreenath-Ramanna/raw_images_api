/*
 * ria_filter.c — spatial filters.
 *
 * Both filters here are separable, which is the only reason they are usable
 * at full resolution: a 2D Gaussian at sigma 2 needs a 13x13 window, 169
 * multiplies per pixel, while two 1D passes need 26. On a 33 MP frame that
 * is the difference between five seconds and half of one.
 */

#include "ria_internal.h"

#include <stdint.h>

/* ── Gaussian kernel ─────────────────────────────────────────────────────── */

/* Three sigma captures 99.7% of the kernel's mass; going wider changes the
 * result by less than one 16-bit level and costs linearly more. */
static ria_status build_kernel(float sigma, float** out_kernel, int* out_radius) {
    const int radius = (int)ceilf(3.0f * sigma);
    const int size = 2 * radius + 1;

    float* k = (float*)malloc((size_t)size * sizeof(float));
    if (!k) return RIA_ERR_MEMORY;

    const float inv = 1.0f / (2.0f * sigma * sigma);
    float total = 0.0f;
    for (int i = -radius; i <= radius; i++) {
        const float v = expf(-(float)(i * i) * inv);
        k[i + radius] = v;
        total += v;
    }
    for (int i = 0; i < size; i++) k[i] /= total;

    *out_kernel = k;
    *out_radius = radius;
    return RIA_OK;
}

/* One separable pass. `horizontal` picks the axis; edges clamp to the last
 * real pixel, which is what keeps a blurred border from darkening. */
/*
 * Generated once per sample type. The alternative — one loop that tests the
 * bit depth as it goes — cannot hoist that test out of the tap loop, because
 * nothing tells the compiler that `src` and `dst` do not alias, so it must
 * reload src->bits on every iteration. At sigma 2 that is 13 redundant loads
 * per pixel per axis.
 */
#define RIA_DEFINE_CONVOLVE(NAME, TYPE)                                       \
static void NAME(const ria_image* src, ria_image* dst, const float* kernel,    \
                 int radius, int horizontal) {                                 \
    const int w = src->width, h = src->height, ch = src->channels;             \
    const float maxv = (float)ria_max_value(src->bits);                        \
    const uint8_t* const sbase = src->data;                                    \
    const size_t sstride = src->stride;                                        \
                                                                               \
    RIA_PARALLEL_ROWS                                                          \
    for (int y = 0; y < h; y++) {                                              \
        TYPE* drow = (TYPE*)(dst->data + (size_t)y * dst->stride);             \
        for (int x = 0; x < w; x++) {                                          \
            float acc[4] = {0.0f, 0.0f, 0.0f, 0.0f};                           \
            for (int k = -radius; k <= radius; k++) {                          \
                const int sx = horizontal ? ria_clampi(x + k, 0, w - 1) : x;   \
                const int sy = horizontal ? y : ria_clampi(y + k, 0, h - 1);   \
                const TYPE* p = (const TYPE*)(sbase + (size_t)sy * sstride) +  \
                                (size_t)sx * (size_t)ch;                       \
                const float weight = kernel[k + radius];                       \
                for (int c = 0; c < ch; c++) acc[c] += weight * (float)p[c];   \
            }                                                                  \
            TYPE* d = drow + (size_t)x * (size_t)ch;                           \
            for (int c = 0; c < ch; c++) {                                     \
                d[c] = (TYPE)ria_clampf(acc[c] + 0.5f, 0.0f, maxv);            \
            }                                                                  \
        }                                                                      \
    }                                                                          \
}

RIA_DEFINE_CONVOLVE(convolve_axis_u8, uint8_t)
RIA_DEFINE_CONVOLVE(convolve_axis_u16, uint16_t)

static void convolve_axis(const ria_image* src, ria_image* dst,
                          const float* kernel, int radius, int horizontal) {
    if (src->bits == 8) {
        convolve_axis_u8(src, dst, kernel, radius, horizontal);
    } else {
        convolve_axis_u16(src, dst, kernel, radius, horizontal);
    }
}

ria_status ria_gaussian_blur(ria_image* img, float sigma) {
    if (!ria_image_valid(img)) return RIA_ERR_INVALID;
    if (sigma <= 0.0f) return RIA_OK;

    float* kernel = NULL;
    int radius = 0;
    ria_status rc = build_kernel(sigma, &kernel, &radius);
    if (rc != RIA_OK) return rc;

    ria_image* tmp = NULL;
    rc = ria_image_like(img, &tmp);
    if (rc != RIA_OK) {
        free(kernel);
        return rc;
    }

    convolve_axis(img, tmp, kernel, radius, 1);
    convolve_axis(tmp, img, kernel, radius, 0);

    ria_image_free(tmp);
    free(kernel);
    return RIA_OK;
}

/* ── Unsharp mask ────────────────────────────────────────────────────────── */

ria_status ria_unsharp_mask(ria_image* img, float sigma, float amount,
                            float threshold) {
    if (!ria_image_valid(img)) return RIA_ERR_INVALID;
    if (sigma <= 0.0f || amount == 0.0f) return RIA_OK;
    if (threshold < 0.0f) return RIA_ERR_INVALID;

    ria_image* blurred = NULL;
    ria_status rc = ria_image_clone(img, &blurred);
    if (rc != RIA_OK) return rc;

    rc = ria_gaussian_blur(blurred, sigma);
    if (rc != RIA_OK) {
        ria_image_free(blurred);
        return rc;
    }

    const int w = img->width, h = img->height, ch = img->channels;
    const int color_ch = ria_color_channels(img);
    const int maxv = ria_max_value(img->bits);
    /* The threshold is quoted in normalised units so that it means the same
     * thing at 8 and 16 bits. */
    const float thr = threshold * (float)maxv;

    RIA_PARALLEL_ROWS
    for (int y = 0; y < h; y++) {
        uint8_t* irow = img->data + (size_t)y * img->stride;
        const uint8_t* brow = blurred->data + (size_t)y * blurred->stride;
        for (int x = 0; x < w; x++) {
            /* Alpha is left alone: sharpening it would carve halos into the
             * edges of a masked composite. */
            for (int c = 0; c < color_ch; c++) {
                float orig, blur;
                if (img->bits == 8) {
                    orig = (float)irow[(size_t)x * ch + c];
                    blur = (float)brow[(size_t)x * ch + c];
                } else {
                    orig = (float)((const uint16_t*)irow)[(size_t)x * ch + c];
                    blur = (float)((const uint16_t*)brow)[(size_t)x * ch + c];
                }
                const float diff = orig - blur;
                if (fabsf(diff) <= thr) continue;

                const float v =
                    ria_clampf(orig + amount * diff, 0.0f, (float)maxv);
                if (img->bits == 8) {
                    irow[(size_t)x * ch + c] = (uint8_t)(v + 0.5f);
                } else {
                    ((uint16_t*)irow)[(size_t)x * ch + c] = (uint16_t)(v + 0.5f);
                }
            }
        }
    }

    ria_image_free(blurred);
    return RIA_OK;
}
