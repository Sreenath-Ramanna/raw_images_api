/*
 * ria_adjust.c — tonal and colour editing, histograms, auto levels.
 *
 * The organising idea: every adjustment that acts on one channel at a time,
 * independently of the others, is a function from input level to output
 * level. There are only 256 or 65536 possible input levels, so the whole
 * chain collapses into a lookup table built once per edit. A 33 MP frame has
 * ~100M samples; evaluating powf() on each of them would take longer than
 * the decode did, and evaluating it 256 times takes no measurable time at
 * all.
 *
 * Only saturation and vibrance escape that, because they need all three
 * channels of a pixel at once. They are folded into the same pass rather
 * than run as a second sweep — memory bandwidth is the budget here, and a
 * second pass over 100 MB costs more than the arithmetic does.
 */

#include "ria_internal.h"

#include <stdint.h>

/* ── Defaults ────────────────────────────────────────────────────────────── */

void ria_adjustments_defaults(ria_adjustments* adj) {
    if (!adj) return;
    memset(adj, 0, sizeof(*adj));
    adj->wb_r = adj->wb_g = adj->wb_b = 1.0f;
    adj->white_point = 1.0f;
    adj->gamma = 1.0f;
}

static int is_neutral(float v, float neutral) {
    const float d = v - neutral;
    return d > -1e-6f && d < 1e-6f;
}

int ria_adjustments_is_identity(const ria_adjustments* adj) {
    if (!adj) return 1;
    return is_neutral(adj->wb_r, 1.0f) && is_neutral(adj->wb_g, 1.0f) &&
           is_neutral(adj->wb_b, 1.0f) && is_neutral(adj->exposure_ev, 0.0f) &&
           is_neutral(adj->black_point, 0.0f) &&
           is_neutral(adj->white_point, 1.0f) &&
           is_neutral(adj->shadows, 0.0f) && is_neutral(adj->highlights, 0.0f) &&
           is_neutral(adj->contrast, 0.0f) && is_neutral(adj->gamma, 1.0f) &&
           is_neutral(adj->saturation, 0.0f) && is_neutral(adj->vibrance, 0.0f);
}

/* ── The per-channel transfer function ───────────────────────────────────── */

/*
 * Shadow and highlight weights. Both peak at 0.59 and fall to zero at the
 * ends, so neither touches true black or true white. The 0.25 scale is not
 * arbitrary: the steepest slope of either weight is 4, and 1 - 0.25*4 = 0,
 * which is exactly the point at which the curve would stop being monotonic
 * and start inverting local contrast. A larger scale would look stronger and
 * be wrong.
 */
#define RIA_TONE_SCALE 0.25f

static inline float shadow_weight(float v) {
    const float t = 1.0f - v;
    return 4.0f * v * t * t;
}

static inline float highlight_weight(float v) {
    return 4.0f * v * v * (1.0f - v);
}

static float transfer(float v, const ria_adjustments* adj, float wb) {
    v *= wb;
    if (adj->exposure_ev != 0.0f) v *= powf(2.0f, adj->exposure_ev);

    const float black = adj->black_point;
    const float white = adj->white_point;
    if (white > black) v = (v - black) / (white - black);

    v = ria_clampf(v, 0.0f, 1.0f);

    if (adj->shadows != 0.0f) {
        v += RIA_TONE_SCALE * adj->shadows * shadow_weight(v);
    }
    if (adj->highlights != 0.0f) {
        v += RIA_TONE_SCALE * adj->highlights * highlight_weight(v);
    }

    if (adj->contrast != 0.0f) {
        v = 0.5f + (v - 0.5f) * (1.0f + adj->contrast);
    }

    v = ria_clampf(v, 0.0f, 1.0f);

    if (adj->gamma > 0.0f && adj->gamma != 1.0f) {
        v = powf(v, 1.0f / adj->gamma);
    }

    return ria_clampf(v, 0.0f, 1.0f);
}

static ria_status build_luts(const ria_image* img, const ria_adjustments* adj,
                             uint16_t** pool, uint16_t* lut[3]) {
    const int levels = 1 << img->bits;
    const float maxv = (float)ria_max_value(img->bits);

    uint16_t* buf = (uint16_t*)malloc((size_t)levels * 3 * sizeof(uint16_t));
    if (!buf) return RIA_ERR_MEMORY;

    lut[0] = buf;
    lut[1] = buf + levels;
    lut[2] = buf + levels * 2;

    const float wb[3] = {adj->wb_r, adj->wb_g, adj->wb_b};
    for (int c = 0; c < 3; c++) {
        for (int i = 0; i < levels; i++) {
            const float v = transfer((float)i / maxv, adj, wb[c]);
            lut[c][i] = (uint16_t)(v * maxv + 0.5f);
        }
    }

    *pool = buf;
    return RIA_OK;
}

/* ── Applying ────────────────────────────────────────────────────────────── */

/* Saturation and vibrance, on values already in [0,maxv]. */
static inline void saturate(float* r, float* g, float* b, float saturation,
                            float vibrance, float maxv) {
    const float y = ria_luma(*r, *g, *b);
    float factor = 1.0f + saturation;

    if (vibrance != 0.0f) {
        /* Weight by how unsaturated the pixel already is, so that a strong
         * vibrance leaves an already-vivid sky alone and lifts flat colour.
         * This is the whole difference between vibrance and saturation. */
        float hi = *r > *g ? *r : *g;
        if (*b > hi) hi = *b;
        float lo = *r < *g ? *r : *g;
        if (*b < lo) lo = *b;
        const float current = hi > 0.0f ? (hi - lo) / hi : 0.0f;
        factor *= 1.0f + vibrance * (1.0f - current);
    }

    *r = ria_clampf(y + (*r - y) * factor, 0.0f, maxv);
    *g = ria_clampf(y + (*g - y) * factor, 0.0f, maxv);
    *b = ria_clampf(y + (*b - y) * factor, 0.0f, maxv);
}

static void apply_pass(ria_image* img, uint16_t* const lut[3],
                       const ria_adjustments* adj, int do_color) {
    const int w = img->width, h = img->height;
    const int ch = img->channels;
    const int color_ch = ria_color_channels(img);
    const float maxv = (float)ria_max_value(img->bits);
    const float saturation = adj ? adj->saturation : 0.0f;
    const float vibrance = adj ? adj->vibrance : 0.0f;

    RIA_PARALLEL_ROWS
    for (int y = 0; y < h; y++) {
        uint8_t* row = img->data + (size_t)y * img->stride;
        if (img->bits == 8) {
            for (int x = 0; x < w; x++) {
                uint8_t* p = row + (size_t)x * (size_t)ch;
                if (color_ch == 1) {
                    p[0] = (uint8_t)lut[1][p[0]];
                    continue;
                }
                float r = (float)lut[0][p[0]];
                float g = (float)lut[1][p[1]];
                float b = (float)lut[2][p[2]];
                if (do_color) saturate(&r, &g, &b, saturation, vibrance, maxv);
                p[0] = (uint8_t)(r + 0.5f);
                p[1] = (uint8_t)(g + 0.5f);
                p[2] = (uint8_t)(b + 0.5f);
            }
        } else {
            uint16_t* rowp = (uint16_t*)row;
            for (int x = 0; x < w; x++) {
                uint16_t* p = rowp + (size_t)x * (size_t)ch;
                if (color_ch == 1) {
                    p[0] = lut[1][p[0]];
                    continue;
                }
                float r = (float)lut[0][p[0]];
                float g = (float)lut[1][p[1]];
                float b = (float)lut[2][p[2]];
                if (do_color) saturate(&r, &g, &b, saturation, vibrance, maxv);
                p[0] = (uint16_t)(r + 0.5f);
                p[1] = (uint16_t)(g + 0.5f);
                p[2] = (uint16_t)(b + 0.5f);
            }
        }
    }
}

ria_status ria_apply_adjustments(ria_image* img, const ria_adjustments* adj) {
    if (!ria_image_valid(img) || !adj) return RIA_ERR_INVALID;
    if (ria_adjustments_is_identity(adj)) return RIA_OK;

    uint16_t* pool = NULL;
    uint16_t* lut[3];
    ria_status rc = build_luts(img, adj, &pool, lut);
    if (rc != RIA_OK) return rc;

    const int do_color = ria_color_channels(img) >= 3 &&
                         (adj->saturation != 0.0f || adj->vibrance != 0.0f);
    apply_pass(img, lut, adj, do_color);

    free(pool);
    return RIA_OK;
}

ria_status ria_apply_curve(ria_image* img, const uint16_t* curve_r,
                           const uint16_t* curve_g, const uint16_t* curve_b) {
    if (!ria_image_valid(img)) return RIA_ERR_INVALID;
    if (!curve_r && !curve_g && !curve_b) return RIA_OK;

    const int levels = 1 << img->bits;
    const int maxv = ria_max_value(img->bits);

    /* Fill in an identity for any channel the caller left out, so the inner
     * loop has no branch per pixel. */
    uint16_t* pool = (uint16_t*)malloc((size_t)levels * sizeof(uint16_t));
    if (!pool) return RIA_ERR_MEMORY;
    for (int i = 0; i < levels; i++) pool[i] = (uint16_t)i;

    uint16_t* lut[3];
    lut[0] = (uint16_t*)(curve_r ? curve_r : pool);
    lut[1] = (uint16_t*)(curve_g ? curve_g : pool);
    lut[2] = (uint16_t*)(curve_b ? curve_b : pool);
    (void)maxv;

    apply_pass(img, lut, NULL, 0);

    free(pool);
    return RIA_OK;
}

/* ── Histogram ───────────────────────────────────────────────────────────── */

ria_status ria_compute_histogram(const ria_image* img, ria_histogram* out) {
    if (!ria_image_valid(img) || !out) return RIA_ERR_INVALID;

    memset(out, 0, sizeof(*out));

    const int w = img->width, h = img->height;
    const int ch = img->channels;
    const int bits = img->bits;
    const int color_ch = ria_color_channels(img);
    /* 16-bit input is folded to 256 bins: a per-level histogram of a 16-bit
     * image is mostly zeroes and no more informative to look at. */
    const int shift = bits == 16 ? 8 : 0;

    uint64_t clipped_black = 0, clipped_white = 0;

    /*
     * Accumulating straight into `out` from several threads would need an
     * atomic per sample, which costs more than the rest of the loop put
     * together. Each thread fills a private copy of the bins instead — 4 KB,
     * comfortably inside L1 — and merges once when it is done, so the
     * synchronised work is 1024 additions per thread rather than four per
     * pixel.
     */
#ifdef _OPENMP
#  pragma omp parallel
#endif
    {
        uint32_t local[4][RIA_HISTOGRAM_BINS];
        uint64_t local_black = 0, local_white = 0;
        memset(local, 0, sizeof(local));

#ifdef _OPENMP
#  pragma omp for schedule(static) nowait
#endif
        for (int y = 0; y < h; y++) {
            const uint8_t* row = img->data + (size_t)y * img->stride;
            for (int x = 0; x < w; x++) {
                int r, g, b;
                if (bits == 8) {
                    const uint8_t* p = row + (size_t)x * (size_t)ch;
                    r = p[0];
                    g = color_ch >= 3 ? p[1] : p[0];
                    b = color_ch >= 3 ? p[2] : p[0];
                } else {
                    const uint16_t* p =
                        (const uint16_t*)row + (size_t)x * (size_t)ch;
                    r = p[0] >> shift;
                    g = color_ch >= 3 ? (p[1] >> shift) : (p[0] >> shift);
                    b = color_ch >= 3 ? (p[2] >> shift) : (p[0] >> shift);
                }
                local[0][r]++;
                local[1][g]++;
                local[2][b]++;
                const int luma =
                    (int)(ria_luma((float)r, (float)g, (float)b) + 0.5f);
                local[3][ria_clampi(luma, 0, RIA_HISTOGRAM_BINS - 1)]++;
                if (r == 0 || g == 0 || b == 0) local_black++;
                if (r == 255 || g == 255 || b == 255) local_white++;
            }
        }

#ifdef _OPENMP
#  pragma omp critical(ria_histogram_merge)
#endif
        {
            for (int i = 0; i < RIA_HISTOGRAM_BINS; i++) {
                out->r[i] += local[0][i];
                out->g[i] += local[1][i];
                out->b[i] += local[2][i];
                out->luma[i] += local[3][i];
            }
            clipped_black += local_black;
            clipped_white += local_white;
        }
    }

    out->pixels = (uint64_t)w * (uint64_t)h;
    if (out->pixels) {
        out->clipped_black = (double)clipped_black / (double)out->pixels;
        out->clipped_white = (double)clipped_white / (double)out->pixels;
    }
    return RIA_OK;
}

/* ── Auto levels ─────────────────────────────────────────────────────────── */

/* Full-precision per-channel histogram: auto levels must find a black point
 * to a single level, and folding a 16-bit image into 256 bins would quantise
 * that to 256 levels of the original scale. */
static ria_status channel_histogram(const ria_image* img, uint32_t** out_hist,
                                    int* out_levels) {
    const int levels = 1 << img->bits;
    uint32_t* hist = (uint32_t*)calloc((size_t)levels * 3, sizeof(uint32_t));
    if (!hist) return RIA_ERR_MEMORY;

    const int ch = img->channels;
    const int color_ch = ria_color_channels(img);

    for (int y = 0; y < img->height; y++) {
        const uint8_t* row = img->data + (size_t)y * img->stride;
        for (int x = 0; x < img->width; x++) {
            if (img->bits == 8) {
                const uint8_t* p = row + (size_t)x * (size_t)ch;
                for (int c = 0; c < 3; c++) {
                    hist[c * levels + p[color_ch >= 3 ? c : 0]]++;
                }
            } else {
                const uint16_t* p = (const uint16_t*)row + (size_t)x * (size_t)ch;
                for (int c = 0; c < 3; c++) {
                    hist[c * levels + p[color_ch >= 3 ? c : 0]]++;
                }
            }
        }
    }

    *out_hist = hist;
    *out_levels = levels;
    return RIA_OK;
}

static void percentile_bounds(const uint32_t* hist, int levels, uint64_t total,
                              double clip_fraction, int* lo, int* hi) {
    const uint64_t target = (uint64_t)(total * clip_fraction);

    uint64_t seen = 0;
    *lo = 0;
    for (int i = 0; i < levels; i++) {
        seen += hist[i];
        if (seen > target) {
            *lo = i;
            break;
        }
    }

    seen = 0;
    *hi = levels - 1;
    for (int i = levels - 1; i >= 0; i--) {
        seen += hist[i];
        if (seen > target) {
            *hi = i;
            break;
        }
    }

    /* A flat channel (a blank frame, a fully clipped one) must not become a
     * division by zero that blows the image out. */
    if (*hi <= *lo) {
        *lo = 0;
        *hi = levels - 1;
    }
}

ria_status ria_auto_levels(ria_image* img, float clip_percent) {
    if (!ria_image_valid(img)) return RIA_ERR_INVALID;
    if (clip_percent < 0.0f || clip_percent >= 50.0f) return RIA_ERR_INVALID;

    uint32_t* hist = NULL;
    int levels = 0;
    ria_status rc = channel_histogram(img, &hist, &levels);
    if (rc != RIA_OK) return rc;

    const uint64_t total = (uint64_t)img->width * (uint64_t)img->height;
    const double fraction = clip_percent / 100.0;
    const int maxv = ria_max_value(img->bits);

    uint16_t* pool = (uint16_t*)malloc((size_t)levels * 3 * sizeof(uint16_t));
    if (!pool) {
        free(hist);
        return RIA_ERR_MEMORY;
    }

    uint16_t* lut[3] = {pool, pool + levels, pool + levels * 2};
    for (int c = 0; c < 3; c++) {
        int lo = 0, hi = levels - 1;
        percentile_bounds(hist + (size_t)c * levels, levels, total, fraction,
                          &lo, &hi);
        const float span = (float)(hi - lo);
        for (int i = 0; i < levels; i++) {
            const float v = ((float)i - (float)lo) / span;
            lut[c][i] = (uint16_t)(ria_clampf(v, 0.0f, 1.0f) * maxv + 0.5f);
        }
    }

    apply_pass(img, lut, NULL, 0);

    free(pool);
    free(hist);
    return RIA_OK;
}

/* ── Suggestion ──────────────────────────────────────────────────────────── */

ria_status ria_suggest_adjustments(const ria_image* img,
                                   ria_adjustments* adj) {
    if (!ria_image_valid(img) || !adj) return RIA_ERR_INVALID;

    ria_histogram hist;
    ria_status rc = ria_compute_histogram(img, &hist);
    if (rc != RIA_OK) return rc;

    ria_adjustments_defaults(adj);
    if (hist.pixels == 0) return RIA_OK;

    /* Black and white points at the 0.1% tails: far enough in to ignore a hot
     * pixel or a lens flare, not so far as to clip real detail. */
    const uint64_t tail = hist.pixels / 1000;
    uint64_t seen = 0;
    int lo = 0, hi = RIA_HISTOGRAM_BINS - 1, median = 128;

    for (int i = 0; i < RIA_HISTOGRAM_BINS; i++) {
        seen += hist.luma[i];
        if (seen > tail) {
            lo = i;
            break;
        }
    }
    seen = 0;
    for (int i = RIA_HISTOGRAM_BINS - 1; i >= 0; i--) {
        seen += hist.luma[i];
        if (seen > tail) {
            hi = i;
            break;
        }
    }
    seen = 0;
    for (int i = 0; i < RIA_HISTOGRAM_BINS; i++) {
        seen += hist.luma[i];
        if (seen >= hist.pixels / 2) {
            median = i;
            break;
        }
    }

    if (hi > lo) {
        adj->black_point = (float)lo / 255.0f;
        adj->white_point = (float)hi / 255.0f;
    }

    /*
     * Aim the median at 0.45 — a middle grey that suits most scenes — and cap
     * the correction at one stop either way. Beyond that the frame is
     * intentionally high or low key, and "fixing" it would be a worse result
     * than leaving it alone.
     */
    const float med = (float)median / 255.0f;
    if (med > 0.001f) {
        const float ev = log2f(0.45f / med);
        adj->exposure_ev = ria_clampf(ev, -1.0f, 1.0f);
    }

    return RIA_OK;
}
