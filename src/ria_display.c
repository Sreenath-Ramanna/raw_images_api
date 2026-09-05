/*
 * ria_display.c — transfer functions and the scene-to-display transform.
 *
 * This file exists because LibRaw does this stage invisibly at the end of a
 * decode, and doing it invisibly is what makes exposure work impossible:
 * by the time pixels come back they have been through a tone curve and the
 * highlights are already clipped. Pulling the stage out into the open costs
 * one function call and buys a domain in which a stop is a stop.
 */

#include "ria_internal.h"

#include <stdint.h>

/* ── Gamma curves ────────────────────────────────────────────────────────── */

/*
 * LibRaw's output curve is not sRGB. It is dcraw's `gamma_curve`, a power
 * function with a linear toe whose breakpoint is solved for numerically so
 * that the two segments meet with a continuous slope. The default (2.222,
 * 4.5) is Rec.709-shaped.
 *
 * This reimplements it rather than approximating it, because the point of
 * RIA_DISPLAY_CLIP is to reproduce a LibRaw decode exactly — a close-but-not
 * equal curve would show up as a systematic few-code-value drift that looks
 * like a bug in whatever is being compared.
 */
typedef struct {
    double g[6];
} gamma_curve;

static void gamma_curve_init(gamma_curve* c, double power, double slope) {
    double* g = c->g;
    double bnd[2] = {0.0, 0.0};

    /* dcraw takes the reciprocal power: 2.222 is expressed as 0.45. */
    g[0] = (power > 0.0) ? 1.0 / power : 0.0;
    g[1] = slope;
    g[2] = g[3] = g[4] = g[5] = 0.0;

    bnd[g[1] >= 1.0] = 1.0;
    if (g[1] != 0.0 && (g[1] - 1.0) * (g[0] - 1.0) <= 0.0) {
        /* Bisection for the toe breakpoint. 48 iterations is dcraw's own
         * count and takes it well past double precision. */
        for (int i = 0; i < 48; i++) {
            g[2] = (bnd[0] + bnd[1]) / 2.0;
            if (g[0] != 0.0) {
                bnd[(pow(g[2] / g[1], -g[0]) - 1.0) / g[0] - 1.0 / g[2] > -1.0] =
                    g[2];
            } else {
                bnd[g[2] / exp(1.0 - 1.0 / g[2]) < g[1]] = g[2];
            }
        }
        g[3] = g[2] / g[1];
        if (g[0] != 0.0) g[4] = g[2] * (1.0 / g[0] - 1.0);
    }
}

static double gamma_curve_encode(const gamma_curve* c, double r) {
    const double* g = c->g;
    if (r <= 0.0) return 0.0;
    if (r >= 1.0) return 1.0;
    if (r < g[3]) return r * g[1];
    if (g[0] != 0.0) return pow(r, g[0]) * (1.0 + g[4]) - g[4];
    return log(r) * g[2] + 1.0;
}

static double gamma_curve_decode(const gamma_curve* c, double v) {
    const double* g = c->g;
    if (v <= 0.0) return 0.0;
    if (v >= 1.0) return 1.0;
    /* The toe ends at r = g[3], i.e. at v = g[3]*g[1]. */
    if (v < g[3] * g[1]) return v / g[1];
    if (g[0] != 0.0) return pow((v + g[4]) / (1.0 + g[4]), 1.0 / g[0]);
    return exp((v - 1.0) / g[2]);
}

/* ── sRGB ────────────────────────────────────────────────────────────────── */

static double srgb_encode(double v) {
    if (v <= 0.0) return 0.0;
    if (v >= 1.0) return 1.0;
    return v <= 0.0031308 ? 12.92 * v : 1.055 * pow(v, 1.0 / 2.4) - 0.055;
}

static double srgb_decode(double v) {
    if (v <= 0.0) return 0.0;
    if (v >= 1.0) return 1.0;
    return v <= 0.04045 ? v / 12.92 : pow((v + 0.055) / 1.055, 2.4);
}

/* ── Scalar entry points ─────────────────────────────────────────────────── */

float ria_transfer_encode(float linear, ria_transfer transfer, float gamma,
                          float slope) {
    switch (transfer) {
        case RIA_TRANSFER_LINEAR:
            return ria_clampf(linear, 0.0f, 1.0f);
        case RIA_TRANSFER_SRGB:
            return (float)srgb_encode(linear);
        case RIA_TRANSFER_GAMMA: {
            gamma_curve c;
            gamma_curve_init(&c, gamma, slope);
            return (float)gamma_curve_encode(&c, linear);
        }
    }
    return ria_clampf(linear, 0.0f, 1.0f);
}

float ria_transfer_decode(float encoded, ria_transfer transfer, float gamma,
                          float slope) {
    switch (transfer) {
        case RIA_TRANSFER_LINEAR:
            return ria_clampf(encoded, 0.0f, 1.0f);
        case RIA_TRANSFER_SRGB:
            return (float)srgb_decode(encoded);
        case RIA_TRANSFER_GAMMA: {
            gamma_curve c;
            gamma_curve_init(&c, gamma, slope);
            return (float)gamma_curve_decode(&c, encoded);
        }
    }
    return ria_clampf(encoded, 0.0f, 1.0f);
}

/* ── Image encoding metadata ─────────────────────────────────────────────── */

ria_status ria_image_set_encoding(ria_image* img, ria_transfer transfer,
                                  float gamma, float slope,
                                  ria_colorspace colorspace) {
    if (!img) return RIA_ERR_INVALID;
    img->transfer = transfer;
    img->transfer_gamma = gamma;
    img->transfer_slope = slope;
    img->colorspace = colorspace;
    return RIA_OK;
}

ria_status ria_image_set_saturation_level(ria_image* img, float level) {
    if (!img || !(level > 0.0f)) return RIA_ERR_INVALID;
    img->saturation_level = level;
    return RIA_OK;
}

/*
 * LibRaw's out_rgb[] table, transcribed from LibRaw_constants in 0.22.2 and
 * indexed by ria_colorspace - 1. It is hardcoded rather than linked because
 * LibRaw_constants is a C++ symbol whose visibility no packaged libraw
 * guarantees. The test suite checks the Adobe RGB row against a matrix fitted
 * from paired real decodes, so the table is checked against LibRaw's actual
 * output rather than against itself.
 */
static const float out_rgb_from_srgb[6][9] = {
    /* SRGB — the identity, because the decode's own base is sRGB. */
    { 1.0f, 0.0f, 0.0f,
      0.0f, 1.0f, 0.0f,
      0.0f, 0.0f, 1.0f },
    /* ADOBE */
    { 0.715146f, 0.284856f, 0.000000f,
      0.000000f, 1.000000f, 0.000000f,
      0.000000f, 0.041166f, 0.958839f },
    /* WIDE */
    { 0.593087f, 0.404710f, 0.002206f,
      0.095413f, 0.843149f, 0.061439f,
      0.011621f, 0.069091f, 0.919288f },
    /* PROPHOTO */
    { 0.529317f, 0.330092f, 0.140588f,
      0.098368f, 0.873465f, 0.028169f,
      0.016879f, 0.117663f, 0.865457f },
    /* XYZ */
    { 0.412456f, 0.357576f, 0.180438f,
      0.212673f, 0.715152f, 0.072175f,
      0.019334f, 0.119192f, 0.950304f },
    /* ACES */
    { 0.439680f, 0.382953f, 0.177367f,
      0.089790f, 0.813433f, 0.096777f,
      0.017548f, 0.111562f, 0.870890f }
};

ria_status ria_colorspace_from_srgb(ria_colorspace space, float matrix[9]) {
    /* RIA_COLORSPACE_RAW is camera primaries, which vary per body and are not
     * in this table at all — refuse rather than return a plausible identity. */
    if (!matrix || (int)space < 1 || (int)space > 6) return RIA_ERR_INVALID;
    memcpy(matrix, out_rgb_from_srgb[(int)space - 1], 9 * sizeof(float));
    return RIA_OK;
}

/* ── The display transform ───────────────────────────────────────────────── */

void ria_display_transform_defaults(ria_display_transform* dt) {
    if (!dt) return;
    dt->mode = RIA_DISPLAY_CLIP;
    /* 0.18 in, 0.18 out: the scale is 1 and the tonal mapping is the
     * identity, so the defaults reproduce a LibRaw decode exactly. */
    dt->grey_point = RIA_MIDDLE_GREY;
    dt->white_point = 1.0f;
    dt->transfer = RIA_TRANSFER_GAMMA;
    dt->gamma = 2.222f;
    dt->slope = 4.5f;
}

/*
 * Extended Reinhard. f(W) = 1 exactly, f is monotonic, and f(y) -> y as
 * y -> 0, so shadows pass through untouched and only the top of the range is
 * compressed.
 *
 * At W = 1 this reduces to y(1+y)/(1+y) = y, the identity — which is why the
 * shoulder mode with default parameters is indistinguishable from clipping,
 * and why engaging it requires giving it something above display white to
 * work with.
 */
static double reinhard(double y, double w) {
    if (w <= 0.0) return y;
    return y * (1.0 + y / (w * w)) / (1.0 + y);
}

ria_status ria_apply_display_transform(const ria_image* scene,
                                       const ria_display_transform* dt,
                                       ria_pixel_format out_fmt,
                                       ria_image** out) {
    if (!ria_image_valid(scene) || !out) return RIA_ERR_INVALID;
    if (ria_format_channels(out_fmt) == 0) return RIA_ERR_INVALID;

    ria_display_transform defaults;
    if (!dt) {
        ria_display_transform_defaults(&defaults);
        dt = &defaults;
    }
    if (dt->grey_point <= 0.0f) return RIA_ERR_INVALID;

    /*
     * Refuse display-referred input rather than transforming it twice. The
     * result of doing so is washed-out and plausible — easy to mistake for a
     * flat photograph rather than a bug — so silence here would be expensive.
     */
    if (scene->transfer != RIA_TRANSFER_LINEAR) return RIA_ERR_INVALID;

    /* Channel counts must agree or the mapping is ambiguous: this stage
     * changes encoding, not layout. Use ria_image_convert for that. */
    const int out_ch = ria_format_channels(out_fmt);
    if (out_ch != scene->channels) return RIA_ERR_INVALID;

    ria_status rc = ria_image_alloc(scene->width, scene->height, out_fmt, NULL,
                                    0, out);
    if (rc != RIA_OK) return rc;

    ria_image* dst = *out;
    dst->pending_flip = scene->pending_flip;
    ria_image_set_encoding(dst, dt->transfer, dt->gamma, dt->slope,
                           scene->colorspace);

    /*
     * The whole transform is a function of one sample value, so tabulate it
     * over every possible input level: 256 or 65536 entries, built once. The
     * per-pixel cost is then a single lookup, and the pow() calls happen a
     * few hundred times instead of a hundred million.
     */
    const int levels = 1 << scene->bits;
    const int in_max = ria_max_value(scene->bits);
    const int out_max = ria_max_value(dst->bits);

    uint16_t* lut = (uint16_t*)malloc((size_t)levels * sizeof(uint16_t));
    if (!lut) {
        ria_image_free(dst);
        *out = NULL;
        return RIA_ERR_MEMORY;
    }

    gamma_curve curve;
    if (dt->transfer == RIA_TRANSFER_GAMMA) {
        gamma_curve_init(&curve, dt->gamma, dt->slope);
    }

    const double scale = RIA_MIDDLE_GREY / (double)dt->grey_point;
    const double w = (double)dt->white_point * scale;

    for (int i = 0; i < levels; i++) {
        double y = ((double)i / (double)in_max) * scale;

        if (dt->mode == RIA_DISPLAY_SHOULDER) y = reinhard(y, w);
        y = y < 0.0 ? 0.0 : (y > 1.0 ? 1.0 : y);

        double v;
        switch (dt->transfer) {
            case RIA_TRANSFER_SRGB:  v = srgb_encode(y); break;
            case RIA_TRANSFER_GAMMA: v = gamma_curve_encode(&curve, y); break;
            default:                 v = y; break;
        }
        lut[i] = (uint16_t)(v * out_max + 0.5);
    }

    const int ch = scene->channels;
    const int w_px = scene->width, h_px = scene->height;

    RIA_PARALLEL_ROWS
    for (int y = 0; y < h_px; y++) {
        const uint8_t* srow = scene->data + (size_t)y * scene->stride;
        uint8_t* drow = dst->data + (size_t)y * dst->stride;
        if (scene->bits == 8) {
            if (dst->bits == 8) {
                for (int i = 0; i < w_px * ch; i++) drow[i] = (uint8_t)lut[srow[i]];
            } else {
                uint16_t* d = (uint16_t*)drow;
                for (int i = 0; i < w_px * ch; i++) d[i] = lut[srow[i]];
            }
        } else {
            const uint16_t* s = (const uint16_t*)srow;
            if (dst->bits == 8) {
                for (int i = 0; i < w_px * ch; i++) drow[i] = (uint8_t)lut[s[i]];
            } else {
                uint16_t* d = (uint16_t*)drow;
                for (int i = 0; i < w_px * ch; i++) d[i] = lut[s[i]];
            }
        }
    }

    free(lut);
    return RIA_OK;
}
