/*
 * test_ria.c — self-contained checks for everything that does not need a
 * camera file, plus an optional pass over real RAW files.
 *
 *   ./ria_tests                 synthetic tests only
 *   ./ria_tests a.NEF b.CR3     also exercises the decode paths
 *
 * The synthetic tests are the ones that run in CI. They are written against
 * properties that must hold (a rotation composed with its inverse is the
 * identity, a resize of a flat field is flat, a monotonic curve stays
 * monotonic) rather than against golden values, so they keep their meaning
 * if an implementation is replaced.
 */

#include "raw_images_api.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_failures = 0;
static int g_checks = 0;

#define CHECK(cond, ...)                                                   \
    do {                                                                   \
        g_checks++;                                                        \
        if (!(cond)) {                                                     \
            g_failures++;                                                  \
            printf("  FAIL %s:%d: ", __FILE__, __LINE__);                  \
            printf(__VA_ARGS__);                                           \
            printf("\n");                                                  \
        }                                                                  \
    } while (0)

#define CHECK_OK(expr) CHECK((expr) == RIA_OK, #expr " should succeed")

static void section(const char* name) { printf("%s\n", name); }

/* ── Helpers ─────────────────────────────────────────────────────────────── */

static ria_image* make_gradient(int w, int h, ria_pixel_format fmt) {
    ria_image* img = NULL;
    if (ria_image_new(w, h, fmt, &img) != RIA_OK) return NULL;
    const int maxv = img->bits == 16 ? 65535 : 255;
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            const int v = (w > 1) ? (x * maxv) / (w - 1) : 0;
            for (int c = 0; c < img->channels; c++) {
                const int s = (c == 3) ? maxv : v;
                if (img->bits == 8) {
                    img->data[(size_t)y * img->stride + x * img->channels + c] =
                        (uint8_t)s;
                } else {
                    ((uint16_t*)(img->data + (size_t)y * img->stride))
                        [x * img->channels + c] = (uint16_t)s;
                }
            }
        }
    }
    return img;
}

static int px8(const ria_image* img, int x, int y, int c) {
    return img->data[(size_t)y * img->stride + (size_t)x * img->channels + c];
}

static void set8(ria_image* img, int x, int y, int r, int g, int b) {
    uint8_t* p = img->data + (size_t)y * img->stride + (size_t)x * img->channels;
    p[0] = (uint8_t)r;
    if (img->channels >= 3) {
        p[1] = (uint8_t)g;
        p[2] = (uint8_t)b;
    }
    if (img->channels == 4) p[3] = 255;
}

/* ── Tests ───────────────────────────────────────────────────────────────── */

static void test_basics(void) {
    section("version and formats");

    CHECK(strlen(ria_version_string()) > 0, "version string is empty");
    CHECK(strlen(ria_libraw_version_string()) > 0, "libraw version is empty");
    CHECK(strcmp(ria_status_string(RIA_OK), "ok") == 0, "status string");

    CHECK(ria_format_channels(RIA_FMT_RGB8) == 3, "RGB8 channels");
    CHECK(ria_format_channels(RIA_FMT_RGBA16) == 4, "RGBA16 channels");
    CHECK(ria_format_bits(RIA_FMT_RGBA16) == 16, "RGBA16 bits");
    CHECK(ria_format_has_alpha(RIA_FMT_RGBA8), "RGBA8 has alpha");
    CHECK(!ria_format_has_alpha(RIA_FMT_RGB8), "RGB8 has no alpha");

    CHECK(ria_is_raw_extension("/photos/DSC_0001.NEF"), "uppercase NEF");
    CHECK(ria_is_raw_extension("a.cr3"), "lowercase cr3");
    CHECK(!ria_is_raw_extension("holiday.jpg"), "jpg is not raw");
    CHECK(!ria_is_raw_extension("noextension"), "no extension");
    /* A directory called shoot.raw must not make its contents look like RAW. */
    CHECK(!ria_is_raw_extension("/shoot.raw/notes"), "extension in a directory");
}

static void test_image_lifecycle(void) {
    section("image allocation");

    ria_image* img = NULL;
    CHECK_OK(ria_image_new(4, 3, RIA_FMT_RGBA8, &img));
    CHECK(img->width == 4 && img->height == 3, "dimensions");
    CHECK(img->channels == 4 && img->bits == 8, "format fields");
    CHECK(img->stride == 16, "stride is %zu, expected 16", img->stride);
    CHECK(img->data_size == 48, "data_size is %zu", img->data_size);

    ria_image* copy = NULL;
    memset(img->data, 0xAB, img->data_size);
    CHECK_OK(ria_image_clone(img, &copy));
    CHECK(memcmp(copy->data, img->data, img->data_size) == 0, "clone matches");
    ria_image_free(copy);
    ria_image_free(img);

    CHECK(ria_image_new(0, 10, RIA_FMT_RGB8, &img) == RIA_ERR_INVALID,
          "zero width is rejected");
    CHECK(ria_image_new(-1, 10, RIA_FMT_RGB8, &img) == RIA_ERR_INVALID,
          "negative width is rejected");

    /* A wrapped buffer must not be freed by the library. */
    uint8_t stack_pixels[12] = {0};
    ria_image* wrapped = NULL;
    CHECK_OK(ria_image_wrap(stack_pixels, 2, 2, RIA_FMT_RGB8, &wrapped));
    CHECK(wrapped->data == stack_pixels, "wrap does not copy");
    ria_image_free(wrapped);
    CHECK(stack_pixels[0] == 0, "wrapped buffer survives free");

    ria_image_free(NULL); /* must not crash */
}

static void test_convert(void) {
    section("format conversion");

    ria_image* rgb = make_gradient(8, 4, RIA_FMT_RGB8);
    CHECK(rgb != NULL, "gradient allocated");

    ria_image* rgba = NULL;
    CHECK_OK(ria_image_convert(rgb, RIA_FMT_RGBA8, &rgba));
    CHECK(rgba->channels == 4, "converted channels");
    for (int x = 0; x < 8; x++) {
        CHECK(px8(rgba, x, 0, 0) == px8(rgb, x, 0, 0), "red preserved at %d", x);
        CHECK(px8(rgba, x, 0, 3) == 255, "alpha opaque at %d", x);
    }

    /* 8 -> 16 -> 8 must be lossless: 257 scales 255 exactly onto 65535. */
    ria_image* wide = NULL;
    ria_image* back = NULL;
    CHECK_OK(ria_image_convert(rgb, RIA_FMT_RGB16, &wide));
    CHECK_OK(ria_image_convert(wide, RIA_FMT_RGB8, &back));
    CHECK(memcmp(back->data, rgb->data, rgb->data_size) == 0,
          "8->16->8 round trip is lossless");

    /* Grey of a neutral pixel is that pixel's value. */
    ria_image* neutral = NULL;
    CHECK_OK(ria_image_new(1, 1, RIA_FMT_RGB8, &neutral));
    set8(neutral, 0, 0, 130, 130, 130);
    ria_image* gray = NULL;
    CHECK_OK(ria_image_convert(neutral, RIA_FMT_GRAY8, &gray));
    CHECK(abs(px8(gray, 0, 0, 0) - 130) <= 1, "neutral luma is %d",
          px8(gray, 0, 0, 0));

    ria_image_free(neutral);
    ria_image_free(gray);
    ria_image_free(back);
    ria_image_free(wide);
    ria_image_free(rgba);
    ria_image_free(rgb);

    /* The hand-written fast path must agree with the general one. */
    uint8_t src[9] = {1, 2, 3, 4, 5, 6, 7, 8, 9};
    uint8_t dst[12] = {0};
    ria_expand_rgb_to_rgba(src, dst, 3);
    for (int i = 0; i < 3; i++) {
        CHECK(dst[i * 4 + 0] == src[i * 3 + 0], "expand r %d", i);
        CHECK(dst[i * 4 + 1] == src[i * 3 + 1], "expand g %d", i);
        CHECK(dst[i * 4 + 2] == src[i * 3 + 2], "expand b %d", i);
        CHECK(dst[i * 4 + 3] == 255, "expand a %d", i);
    }
}

static void test_geometry(void) {
    section("crop, orientation, resize");

    ria_image* img = make_gradient(16, 8, RIA_FMT_RGB8);

    ria_image* cropped = NULL;
    CHECK_OK(ria_crop(img, 4, 2, 5, 3, &cropped));
    CHECK(cropped->width == 5 && cropped->height == 3, "crop dimensions");
    CHECK(px8(cropped, 0, 0, 0) == px8(img, 4, 2, 0), "crop origin");
    CHECK(ria_crop(img, 14, 0, 5, 3, &cropped) == RIA_ERR_INVALID,
          "crop past the right edge is rejected");
    ria_image_free(cropped);

    /* A quarter turn and its inverse must compose to the identity — the
     * cheapest way to catch a transposed index. */
    ria_image* cw = NULL;
    ria_image* back = NULL;
    CHECK_OK(ria_apply_orientation(img, RIA_FLIP_90_CW, &cw));
    CHECK(cw->width == img->height && cw->height == img->width,
          "quarter turn transposes: %dx%d", cw->width, cw->height);
    CHECK_OK(ria_apply_orientation(cw, RIA_FLIP_90_CCW, &back));
    CHECK(back->width == img->width && back->height == img->height,
          "round trip dimensions");
    CHECK(memcmp(back->data, img->data, img->data_size) == 0,
          "90 CW then 90 CCW is the identity");
    ria_image_free(cw);
    ria_image_free(back);

    ria_image* half = NULL;
    ria_image* full = NULL;
    CHECK_OK(ria_apply_orientation(img, RIA_FLIP_180, &half));
    CHECK_OK(ria_apply_orientation(half, RIA_FLIP_180, &full));
    CHECK(memcmp(full->data, img->data, img->data_size) == 0,
          "180 twice is the identity");
    ria_image_free(half);
    ria_image_free(full);

    /* pending_flip must be cleared, or a consumer will rotate twice. */
    ria_image* oriented = NULL;
    img->pending_flip = RIA_FLIP_90_CW;
    CHECK_OK(ria_apply_orientation(img, RIA_FLIP_90_CW, &oriented));
    CHECK(oriented->pending_flip == RIA_FLIP_NONE, "orientation clears the flag");
    ria_image_free(oriented);
    img->pending_flip = RIA_FLIP_NONE;

    ria_image_free(img);

    /* A flat field must survive a resize unchanged. Ringing or an off-by-one
     * in the filter weights shows up here immediately. */
    ria_image* flat = NULL;
    CHECK_OK(ria_image_new(64, 64, RIA_FMT_RGB8, &flat));
    memset(flat->data, 200, flat->data_size);
    ria_image* smaller = NULL;
    CHECK_OK(ria_resize(flat, 17, 9, RIA_RESIZE_TRIANGLE, &smaller));
    CHECK(smaller->width == 17 && smaller->height == 9, "resize dimensions");
    int flat_ok = 1;
    for (size_t i = 0; i < smaller->data_size; i++) {
        if (smaller->data[i] != 200) flat_ok = 0;
    }
    CHECK(flat_ok, "downscale of a flat field stays flat");
    ria_image_free(smaller);

    ria_image* larger = NULL;
    CHECK_OK(ria_resize(flat, 100, 100, RIA_RESIZE_TRIANGLE, &larger));
    int up_ok = 1;
    for (size_t i = 0; i < larger->data_size; i++) {
        if (larger->data[i] != 200) up_ok = 0;
    }
    CHECK(up_ok, "upscale of a flat field stays flat");
    ria_image_free(larger);
    ria_image_free(flat);

    /* A downscaled gradient must still be a gradient, left to right. */
    ria_image* grad = make_gradient(512, 8, RIA_FMT_RGB8);
    ria_image* small = NULL;
    CHECK_OK(ria_resize(grad, 32, 4, RIA_RESIZE_TRIANGLE, &small));
    int monotone = 1;
    for (int x = 1; x < small->width; x++) {
        if (px8(small, x, 0, 0) < px8(small, x - 1, 0, 0)) monotone = 0;
    }
    CHECK(monotone, "downscaled gradient stays monotonic");
    CHECK(px8(small, 0, 0, 0) < 40, "left end stays dark: %d",
          px8(small, 0, 0, 0));
    CHECK(px8(small, small->width - 1, 0, 0) > 215, "right end stays bright: %d",
          px8(small, small->width - 1, 0, 0));
    ria_image_free(small);

    ria_image* fitted = NULL;
    CHECK_OK(ria_fit_within(grad, 100, 100, RIA_RESIZE_TRIANGLE, &fitted));
    CHECK(fitted->width == 100, "fit longest edge: %d", fitted->width);
    CHECK(fitted->height == 2, "fit preserves aspect: %d", fitted->height);
    ria_image_free(fitted);

    ria_image* untouched = NULL;
    CHECK_OK(ria_fit_within(grad, 4000, 4000, RIA_RESIZE_TRIANGLE, &untouched));
    CHECK(untouched->width == grad->width, "fit never enlarges");
    ria_image_free(untouched);
    ria_image_free(grad);
}

static void test_adjustments(void) {
    section("tonal adjustments");

    ria_adjustments adj;
    ria_adjustments_defaults(&adj);
    CHECK(ria_adjustments_is_identity(&adj), "defaults are the identity");

    ria_image* img = make_gradient(256, 4, RIA_FMT_RGB8);
    ria_image* before = NULL;
    ria_image_clone(img, &before);

    CHECK_OK(ria_apply_adjustments(img, &adj));
    CHECK(memcmp(img->data, before->data, img->data_size) == 0,
          "identity adjustment changes nothing");

    /*
     * There used to be an exposure test here asserting that +1 EV on an
     * encoded 64 gives 128. That is the encoded-domain doubling, and it is
     * wrong: 64/255 encoded through a 2.222 gamma is 0.0498 linear, and one
     * stop up is 0.0996, which re-encodes to 90 — not 128. The assertion was
     * pinning the defect in place, so both it and the field it tested are
     * gone. Exposure now lives in the scene-referred domain; the round trip
     * through the transfer functions is checked in test_transfer(), and the
     * linear-light behaviour in test_display_transform().
     */

    /* Gamma is the display-domain midtone control, and it must fix both
     * endpoints while moving what lies between them. */
    ria_adjustments_defaults(&adj);
    adj.gamma = 2.0f;
    CHECK_OK(ria_apply_adjustments(img, &adj));
    CHECK(px8(img, 0, 0, 0) == 0, "gamma fixes black: %d", px8(img, 0, 0, 0));
    CHECK(px8(img, 255, 0, 0) == 255, "gamma fixes white: %d",
          px8(img, 255, 0, 0));
    CHECK(px8(img, 128, 0, 0) > 150, "gamma 2.0 lifts the midtones: %d",
          px8(img, 128, 0, 0));
    ria_image_free(img);

    /* Any curve built from these controls must stay non-decreasing: an
     * inverted segment would show as posterised, reversed local contrast. */
    const float shadow_values[] = {-1.0f, -0.5f, 0.0f, 0.5f, 1.0f};
    for (size_t s = 0; s < sizeof(shadow_values) / sizeof(shadow_values[0]); s++) {
        for (size_t h = 0; h < sizeof(shadow_values) / sizeof(shadow_values[0]); h++) {
            ria_image* ramp = make_gradient(256, 1, RIA_FMT_RGB8);
            ria_adjustments_defaults(&adj);
            adj.shadows = shadow_values[s];
            adj.highlights = shadow_values[h];
            CHECK_OK(ria_apply_adjustments(ramp, &adj));
            int ok = 1;
            for (int x = 1; x < 256; x++) {
                if (px8(ramp, x, 0, 0) < px8(ramp, x - 1, 0, 0)) ok = 0;
            }
            CHECK(ok, "shadows %.1f highlights %.1f stays monotonic",
                  (double)shadow_values[s], (double)shadow_values[h]);
            ria_image_free(ramp);
        }
    }

    /* Full desaturation must land every channel on the luma. */
    ria_image* color = NULL;
    CHECK_OK(ria_image_new(1, 1, RIA_FMT_RGB8, &color));
    set8(color, 0, 0, 200, 100, 50);
    ria_adjustments_defaults(&adj);
    adj.saturation = -1.0f;
    CHECK_OK(ria_apply_adjustments(color, &adj));
    CHECK(abs(px8(color, 0, 0, 0) - px8(color, 0, 0, 1)) <= 1 &&
          abs(px8(color, 0, 0, 1) - px8(color, 0, 0, 2)) <= 1,
          "saturation -1 gives neutral: %d %d %d", px8(color, 0, 0, 0),
          px8(color, 0, 0, 1), px8(color, 0, 0, 2));
    ria_image_free(color);

    /* Vibrance must move a flat colour more than an already-vivid one. */
    ria_image* pair = NULL;
    CHECK_OK(ria_image_new(2, 1, RIA_FMT_RGB8, &pair));
    set8(pair, 0, 0, 140, 128, 128);  /* barely coloured                     */
    set8(pair, 1, 0, 250, 20, 20);    /* saturated                           */
    const int flat_before = px8(pair, 0, 0, 0) - px8(pair, 0, 0, 1);
    const int vivid_before = px8(pair, 1, 0, 0) - px8(pair, 1, 0, 1);
    ria_adjustments_defaults(&adj);
    adj.vibrance = 1.0f;
    CHECK_OK(ria_apply_adjustments(pair, &adj));
    const int flat_delta = (px8(pair, 0, 0, 0) - px8(pair, 0, 0, 1)) - flat_before;
    const int vivid_delta = (px8(pair, 1, 0, 0) - px8(pair, 1, 0, 1)) - vivid_before;
    CHECK(flat_delta > 0, "vibrance lifts a flat colour (%d)", flat_delta);
    CHECK((double)flat_delta / (flat_before + 1) >
              (double)vivid_delta / (vivid_before + 1),
          "vibrance favours the flat colour: %d vs %d", flat_delta, vivid_delta);
    ria_image_free(pair);

    ria_image_free(before);
}

static void test_transfer(void) {
    section("transfer functions");

    const ria_transfer kinds[] = {RIA_TRANSFER_SRGB, RIA_TRANSFER_GAMMA};
    for (unsigned k = 0; k < 2; k++) {
        for (int i = 0; i <= 20; i++) {
            const float v = (float)i / 20.0f;
            const float enc = ria_transfer_encode(v, kinds[k], 2.222f, 4.5f);
            const float back = ria_transfer_decode(enc, kinds[k], 2.222f, 4.5f);
            CHECK(fabsf(back - v) < 1e-4f,
                  "transfer %u round trip at %.2f gave %.5f", k, (double)v,
                  (double)back);
        }
        /* Endpoints must be exact, or black and white drift on every pass. */
        CHECK(ria_transfer_encode(0.0f, kinds[k], 2.222f, 4.5f) == 0.0f,
              "transfer %u encodes 0 to 0", k);
        CHECK(ria_transfer_encode(1.0f, kinds[k], 2.222f, 4.5f) == 1.0f,
              "transfer %u encodes 1 to 1", k);
        /* Encoding must brighten: it exists to spend code values on shadows. */
        CHECK(ria_transfer_encode(0.18f, kinds[k], 2.222f, 4.5f) > 0.35f,
              "transfer %u lifts middle grey above 0.35", k);
    }

    CHECK(ria_transfer_encode(0.5f, RIA_TRANSFER_LINEAR, 0, 0) == 0.5f,
          "linear transfer is the identity");

    /*
     * The concrete statement of why exposure_ev was removed.
     *
     * An encoded 64 is 0.0787 in linear light; one stop up is 0.1575, which
     * re-encodes to 97. The old display-domain implementation doubled the
     * encoded value and returned 128. At mid-grey the same error takes 128 to
     * 255 where the correct answer is 184 — the naive result is 0.93 EV too
     * bright, and above encoded 192 it is pinned at white with no headroom
     * left at all.
     *
     * Note these are *not* the values a pure 2.222 power function gives.
     * LibRaw's curve has a linear toe of slope 4.5, which lifts the shadows
     * considerably; ignoring it puts the mid-grey figure at 174 rather than
     * 184. The expected values here come from the real curve.
     */
    const struct { int in, expect; } ev_cases[] = {
        {32, 53}, {64, 97}, {96, 140}, {128, 184}, {160, 228},
    };
    for (unsigned i = 0; i < sizeof(ev_cases) / sizeof(ev_cases[0]); i++) {
        const float e = (float)ev_cases[i].in / 255.0f;
        const float lin = ria_transfer_decode(e, RIA_TRANSFER_GAMMA, 2.222f,
                                              4.5f);
        const float up = ria_transfer_encode(lin * 2.0f, RIA_TRANSFER_GAMMA,
                                             2.222f, 4.5f);
        const int code = (int)(up * 255.0f + 0.5f);
        CHECK(abs(code - ev_cases[i].expect) <= 1,
              "+1 EV on encoded %d gives %d, expected %d", ev_cases[i].in, code,
              ev_cases[i].expect);
        /* The whole point: linear-correct is always darker than doubling. */
        const int naive = ev_cases[i].in * 2 > 255 ? 255 : ev_cases[i].in * 2;
        CHECK(code < naive, "and stays below the encoded doubling of %d", naive);
    }
}

static void test_display_transform(void) {
    section("display transform");

    ria_display_transform dt;
    ria_display_transform_defaults(&dt);
    CHECK(dt.mode == RIA_DISPLAY_CLIP, "defaults to clipping");
    CHECK(fabsf(dt.grey_point - 0.18f) < 1e-6f, "grey point is 0.18");
    CHECK(fabsf(dt.white_point - 1.0f) < 1e-6f, "white point is 1.0");

    /* A linear ramp through the default transform must equal encoding it. */
    ria_image* scene = make_gradient(256, 1, RIA_FMT_RGB8);
    ria_image_set_encoding(scene, RIA_TRANSFER_LINEAR, 1.0f, 1.0f,
                           RIA_COLORSPACE_SRGB);
    ria_image* shown = NULL;
    CHECK_OK(ria_apply_display_transform(scene, &dt, RIA_FMT_RGB8, &shown));
    CHECK(shown->transfer == RIA_TRANSFER_GAMMA, "output is labelled encoded");

    int worst = 0;
    for (int x = 0; x < 256; x++) {
        const float expect = ria_transfer_encode((float)x / 255.0f,
                                                 RIA_TRANSFER_GAMMA, 2.222f,
                                                 4.5f);
        const int d = abs(px8(shown, x, 0, 0) - (int)(expect * 255.0f + 0.5f));
        if (d > worst) worst = d;
    }
    CHECK(worst <= 1, "default transform is the encode curve (worst diff %d)",
          worst);
    ria_image_free(shown);

    /* Halving the grey point is exactly one stop of brightening. */
    ria_display_transform bright = dt;
    bright.grey_point = 0.09f;
    ria_image* lifted = NULL;
    CHECK_OK(ria_apply_display_transform(scene, &bright, RIA_FMT_RGB8, &lifted));
    const float lin_in = 64.0f / 255.0f;
    const float expect = ria_transfer_encode(lin_in * 2.0f, RIA_TRANSFER_GAMMA,
                                             2.222f, 4.5f);
    CHECK(abs(px8(lifted, 64, 0, 0) - (int)(expect * 255.0f + 0.5f)) <= 1,
          "half grey point doubles the linear value: %d", px8(lifted, 64, 0, 0));
    ria_image_free(lifted);

    /* At white_point 1.0 the shoulder is mathematically the identity, so it
     * must agree with clipping exactly — this pins the W=1 reduction. */
    ria_display_transform shoulder = dt;
    shoulder.mode = RIA_DISPLAY_SHOULDER;
    ria_image* a = NULL;
    ria_image* b = NULL;
    CHECK_OK(ria_apply_display_transform(scene, &dt, RIA_FMT_RGB8, &a));
    CHECK_OK(ria_apply_display_transform(scene, &shoulder, RIA_FMT_RGB8, &b));
    CHECK(memcmp(a->data, b->data, a->data_size) == 0,
          "shoulder at white 1.0 is identical to clipping");
    ria_image_free(a);
    ria_image_free(b);

    /*
     * Given something above display white to work with, the shoulder must
     * differ from clipping and must preserve distinctions in the highlights
     * rather than flattening them. This is the check that catches a
     * materialised intermediate clamping the input.
     */
    shoulder.white_point = 4.0f;
    ria_display_transform clip4 = dt;
    ria_image* rolled = NULL;
    ria_image* clipped = NULL;
    /* Brighten by 2 stops so the top of the ramp lands above white. */
    shoulder.grey_point = clip4.grey_point = 0.18f / 4.0f;
    CHECK_OK(ria_apply_display_transform(scene, &shoulder, RIA_FMT_RGB8, &rolled));
    CHECK_OK(ria_apply_display_transform(scene, &clip4, RIA_FMT_RGB8, &clipped));

    int clip_flat = 0, roll_flat = 0;
    for (int x = 192; x < 256; x++) {
        if (px8(clipped, x, 0, 0) == 255) clip_flat++;
        if (px8(rolled, x, 0, 0) == 255) roll_flat++;
    }
    CHECK(clip_flat > roll_flat,
          "the shoulder keeps highlight detail clipping loses: %d vs %d flat",
          roll_flat, clip_flat);
    CHECK(memcmp(rolled->data, clipped->data, rolled->data_size) != 0,
          "shoulder and clip differ once there is something to roll off");
    ria_image_free(rolled);
    ria_image_free(clipped);

    /* Display-referred input must be refused, not transformed twice. */
    ria_image* encoded = make_gradient(16, 1, RIA_FMT_RGB8);
    ria_image* nope = NULL;
    CHECK(ria_apply_display_transform(encoded, &dt, RIA_FMT_RGB8, &nope) ==
              RIA_ERR_INVALID,
          "a display-referred image is rejected");
    ria_image_free(encoded);

    ria_image_free(scene);
}

static void test_encoding_propagation(void) {
    section("encoding metadata");

    ria_image* img = make_gradient(32, 16, RIA_FMT_RGB8);
    CHECK(img->transfer == RIA_TRANSFER_SRGB,
          "a fresh image defaults to display-referred sRGB");

    CHECK_OK(ria_image_set_encoding(img, RIA_TRANSFER_LINEAR, 1.0f, 1.0f,
                                    RIA_COLORSPACE_ADOBE));

    /* Every operation producing a new image must carry the label across, or
     * a resized scene-referred buffer comes back claiming to be encoded and
     * the next EV operation refuses it for the wrong reason. */
    struct { const char* name; ria_image* out; } cases[8];
    int n = 0;

    ria_image* t = NULL;
    CHECK_OK(ria_image_clone(img, &t));            cases[n].name = "clone";      cases[n++].out = t;
    CHECK_OK(ria_image_convert(img, RIA_FMT_RGBA8, &t)); cases[n].name = "convert"; cases[n++].out = t;
    CHECK_OK(ria_crop(img, 1, 1, 8, 8, &t));       cases[n].name = "crop";       cases[n++].out = t;
    CHECK_OK(ria_resize(img, 8, 4, RIA_RESIZE_TRIANGLE, &t)); cases[n].name = "resize"; cases[n++].out = t;
    CHECK_OK(ria_apply_orientation(img, RIA_FLIP_90_CW, &t)); cases[n].name = "orientation"; cases[n++].out = t;
    CHECK_OK(ria_fit_within(img, 8, 8, RIA_RESIZE_TRIANGLE, &t)); cases[n].name = "fit"; cases[n++].out = t;

    for (int i = 0; i < n; i++) {
        CHECK(cases[i].out->transfer == RIA_TRANSFER_LINEAR,
              "%s preserves the transfer function", cases[i].name);
        CHECK(cases[i].out->colorspace == RIA_COLORSPACE_ADOBE,
              "%s preserves the colourspace", cases[i].name);
        ria_image_free(cases[i].out);
    }

    ria_image_free(img);
}

static void test_statistics(void) {
    section("histogram and auto levels");

    ria_image* img = make_gradient(256, 4, RIA_FMT_RGB8);
    ria_histogram hist;
    CHECK_OK(ria_compute_histogram(img, &hist));
    CHECK(hist.pixels == 256 * 4, "pixel count is %llu",
          (unsigned long long)hist.pixels);
    uint64_t total = 0;
    for (int i = 0; i < RIA_HISTOGRAM_BINS; i++) total += hist.r[i];
    CHECK(total == hist.pixels, "histogram sums to the pixel count");
    CHECK(hist.r[0] == 4, "one column of black per row: %u", hist.r[0]);
    ria_image_free(img);

    /* A gradient squeezed into the middle of the range must come back out to
     * the full range. */
    ria_image* dull = NULL;
    CHECK_OK(ria_image_new(129, 2, RIA_FMT_RGB8, &dull));
    for (int y = 0; y < 2; y++) {
        for (int x = 0; x < 129; x++) set8(dull, x, y, 64 + x / 2, 64 + x / 2,
                                           64 + x / 2);
    }
    CHECK_OK(ria_auto_levels(dull, 0.0f));
    CHECK(px8(dull, 0, 0, 0) == 0, "auto levels reaches black: %d",
          px8(dull, 0, 0, 0));
    CHECK(px8(dull, 128, 0, 0) == 255, "auto levels reaches white: %d",
          px8(dull, 128, 0, 0));
    ria_image_free(dull);

    /* A single flat colour has no range to stretch; it must survive intact
     * rather than divide by zero. */
    ria_image* flat = NULL;
    CHECK_OK(ria_image_new(8, 8, RIA_FMT_RGB8, &flat));
    memset(flat->data, 100, flat->data_size);
    CHECK_OK(ria_auto_levels(flat, 0.5f));
    CHECK(px8(flat, 0, 0, 0) == 100, "flat image survives auto levels: %d",
          px8(flat, 0, 0, 0));
    ria_image_free(flat);

    ria_image* dark = NULL;
    CHECK_OK(ria_image_new(16, 16, RIA_FMT_RGB8, &dark));
    memset(dark->data, 30, dark->data_size);
    ria_adjustments adj;
    CHECK_OK(ria_suggest_adjustments(dark, &adj));
    /* A dark frame should suggest a midtone lift. In the display domain that
     * is a gamma above 1, not an exposure in stops — the value is a code
     * value, not scene light. */
    CHECK(adj.gamma > 1.0f, "a dark frame suggests a midtone lift: %.2f",
          (double)adj.gamma);
    ria_image_free(dark);
}

static void test_filters(void) {
    section("blur and sharpen");

    /* A flat field must be unchanged by either filter — the standard test
     * for kernel normalisation and edge handling in one. */
    ria_image* flat = NULL;
    CHECK_OK(ria_image_new(32, 32, RIA_FMT_RGB8, &flat));
    memset(flat->data, 123, flat->data_size);
    CHECK_OK(ria_gaussian_blur(flat, 2.0f));
    int ok = 1;
    for (size_t i = 0; i < flat->data_size; i++) {
        if (flat->data[i] != 123) ok = 0;
    }
    CHECK(ok, "blur leaves a flat field alone, including at the edges");

    CHECK_OK(ria_unsharp_mask(flat, 1.0f, 1.0f, 0.0f));
    ok = 1;
    for (size_t i = 0; i < flat->data_size; i++) {
        if (flat->data[i] != 123) ok = 0;
    }
    CHECK(ok, "unsharp leaves a flat field alone");
    ria_image_free(flat);

    /* Blur must soften a hard edge, and unsharp must steepen it back. */
    ria_image* edge = NULL;
    CHECK_OK(ria_image_new(32, 8, RIA_FMT_RGB8, &edge));
    for (int y = 0; y < 8; y++) {
        for (int x = 0; x < 32; x++) {
            const int v = x < 16 ? 40 : 210;
            set8(edge, x, y, v, v, v);
        }
    }
    ria_image* blurred = NULL;
    ria_image_clone(edge, &blurred);
    CHECK_OK(ria_gaussian_blur(blurred, 2.0f));
    const int edge_step = px8(edge, 16, 4, 0) - px8(edge, 15, 4, 0);
    const int blur_step = px8(blurred, 16, 4, 0) - px8(blurred, 15, 4, 0);
    CHECK(blur_step < edge_step, "blur softens the step: %d -> %d", edge_step,
          blur_step);

    CHECK_OK(ria_unsharp_mask(blurred, 2.0f, 1.5f, 0.0f));
    const int sharp_step = px8(blurred, 16, 4, 0) - px8(blurred, 15, 4, 0);
    CHECK(sharp_step > blur_step, "unsharp steepens the step: %d -> %d",
          blur_step, sharp_step);

    /* A threshold above the local difference must suppress the effect. */
    ria_image* quiet = NULL;
    ria_image_clone(edge, &quiet);
    CHECK_OK(ria_gaussian_blur(quiet, 2.0f));
    ria_image* control = NULL;
    ria_image_clone(quiet, &control);
    CHECK_OK(ria_unsharp_mask(quiet, 2.0f, 1.5f, 1.0f));
    CHECK(memcmp(quiet->data, control->data, quiet->data_size) == 0,
          "threshold 1.0 suppresses all sharpening");

    ria_image_free(control);
    ria_image_free(quiet);
    ria_image_free(blurred);
    ria_image_free(edge);
}

static void test_focus_math(void) {
    section("focus point geometry");

    ria_focus_area area;
    ria_focus_point f;
    memset(&f, 0, sizeof(f));

    CHECK(ria_focus_resolve(&f, 100, 100, &area) == RIA_ERR_NO_DATA,
          "an invalid point resolves to no data");

    /* Canon: origin at the centre, so (0,0) is the middle of the frame. */
    f.valid = 1;
    f.vendor = RIA_AF_VENDOR_CANON;
    f.af_image_width = 1000;
    f.af_image_height = 500;
    f.x = 0;
    f.y = 0;
    f.width = 100;
    f.height = 50;
    f.flip = RIA_FLIP_NONE;
    CHECK_OK(ria_focus_resolve(&f, 2000, 1000, &area));
    CHECK(fabs(area.center_x - 1000.0) < 0.01, "canon centre x: %.1f",
          area.center_x);
    CHECK(fabs(area.center_y - 500.0) < 0.01, "canon centre y: %.1f",
          area.center_y);
    CHECK(fabs(area.width - 200.0) < 0.01, "af area scales with the image");

    /* Positive Y is up by default, so it must land above the centre. */
    f.y = 100;
    CHECK_OK(ria_focus_resolve(&f, 2000, 1000, &area));
    CHECK(area.center_y < 500.0, "canon +y is up: %.1f", area.center_y);
    ria_focus_set_canon_y_up(0);
    CHECK_OK(ria_focus_resolve(&f, 2000, 1000, &area));
    CHECK(area.center_y > 500.0, "the switch flips it: %.1f", area.center_y);
    ria_focus_set_canon_y_up(1);
    CHECK(ria_focus_get_canon_y_up() == 1, "switch reads back");

    /* Nikon: origin top-left, unsigned. */
    memset(&f, 0, sizeof(f));
    f.valid = 1;
    f.vendor = RIA_AF_VENDOR_NIKON;
    f.af_image_width = 1000;
    f.af_image_height = 500;
    f.x = 250;
    f.y = 125;
    f.width = 100;
    f.height = 50;
    CHECK_OK(ria_focus_resolve(&f, 1000, 500, &area));
    CHECK(fabs(area.center_x - 250.0) < 0.01, "nikon x passes through");
    CHECK(fabs(area.center_y - 125.0) < 0.01, "nikon y passes through");

    /*
     * A quarter turn: the decoded image is transposed, and a point in the
     * top-left of the sensor must follow the pixels round. For flip 6 (90 CW)
     * the sensor's top-left corner ends up at the top-right.
     */
    f.flip = RIA_FLIP_90_CW;
    f.x = 100;
    f.y = 50;
    CHECK_OK(ria_focus_resolve(&f, 500, 1000, &area));
    CHECK(area.center_x > 250.0, "90 CW moves the point right: %.1f",
          area.center_x);
    CHECK(area.center_y < 500.0, "90 CW keeps it in the upper half: %.1f",
          area.center_y);
    CHECK(area.center_x >= 0 && area.center_x <= 500 && area.center_y >= 0 &&
              area.center_y <= 1000,
          "rotated point stays inside the frame");
}

static void test_pnm(void) {
    section("pnm output");

    ria_image* img = make_gradient(4, 2, RIA_FMT_RGB8);
    const char* path = "ria_test_out.ppm";
    CHECK_OK(ria_write_pnm(img, path));

    FILE* f = fopen(path, "rb");
    CHECK(f != NULL, "output file exists");
    if (f) {
        char header[32] = {0};
        size_t n = fread(header, 1, 15, f);
        CHECK(n > 10 && strncmp(header, "P6\n4 2\n255\n", 11) == 0,
              "ppm header is '%s'", header);
        fclose(f);
        remove(path);
    }

    CHECK(ria_write_pnm(img, "/nonexistent-dir/x.ppm") == RIA_ERR_IO,
          "an unwritable path is an IO error");
    ria_image_free(img);
}

/* ── Optional: real files ────────────────────────────────────────────────── */

static void test_color_data_arguments(void) {
    printf("colour data arguments\n");
    ria_color_data cd;
    CHECK(ria_raw_color_data(NULL, &cd) == RIA_ERR_INVALID,
          "NULL handle is rejected");
    CHECK(ria_raw_color_data((ria_raw*)&cd, NULL) == RIA_ERR_INVALID,
          "NULL output is rejected");
}

static void test_raw_file(const char* path) {
    printf("raw file: %s\n", path);

    ria_raw* raw = NULL;
    ria_status rc = ria_raw_open(path, &raw);
    CHECK(rc == RIA_OK, "open: %s", ria_status_string(rc));
    if (rc != RIA_OK) return;

    ria_metadata meta;
    CHECK_OK(ria_raw_metadata(raw, &meta));
    CHECK(meta.make[0] != '\0', "camera make is populated");
    CHECK(meta.width > 0 && meta.height > 0, "dimensions are populated");
    printf("  %s %s, %d x %d\n", meta.make, meta.model, meta.width, meta.height);

    /* A quarter-turn frame must report transposed dimensions. */
    if (meta.flip == RIA_FLIP_90_CW || meta.flip == RIA_FLIP_90_CCW) {
        CHECK(meta.width == meta.raw_height && meta.height == meta.raw_width,
              "portrait dimensions are transposed");
    }

    ria_focus_point focus;
    rc = ria_raw_focus(raw, &focus);
    CHECK(rc == RIA_OK || rc == RIA_ERR_NO_DATA, "focus read: %s",
          ria_status_string(rc));
    if (rc == RIA_OK) {
        ria_focus_area area;
        CHECK_OK(ria_focus_resolve(&focus, meta.width, meta.height, &area));
        CHECK(area.center_x >= 0 && area.center_x <= meta.width,
              "focus x inside the frame: %.0f", area.center_x);
        CHECK(area.center_y >= 0 && area.center_y <= meta.height,
              "focus y inside the frame: %.0f", area.center_y);
        printf("  focus at (%.0f, %.0f)\n", area.center_x, area.center_y);
    }

    ria_preview* preview = NULL;
    rc = ria_raw_preview(raw, &preview);
    CHECK(rc == RIA_OK || rc == RIA_ERR_NO_DATA, "preview: %s",
          ria_status_string(rc));
    if (rc == RIA_OK) {
        CHECK(preview->data_size > 0, "preview has bytes");
        CHECK(preview->width > 0, "preview has dimensions");
        printf("  preview %d x %d, %zu KB\n", preview->width, preview->height,
               preview->data_size / 1024);
        ria_preview_free(preview);
    }

    ria_color_data cd;
    CHECK_OK(ria_raw_color_data(raw, &cd));
    CHECK(cd.colors == meta.colors, "colour data agrees with metadata");
    CHECK(cd.cam_mul[1] > 0.0f, "as-shot green multiplier is populated");
    /*
     * cam_xyz is a colour matrix, so its 3x3 leading block must be invertible
     * for the camera neutral to be taken back to XYZ at all. A zero
     * determinant means LibRaw has no characterisation for this body, and
     * every colorimetric route downstream is meaningless.
     */
    {
        const float(*m)[3] = cd.cam_xyz;
        const double det =
            (double)m[0][0] * (m[1][1] * m[2][2] - m[1][2] * m[2][1]) -
            (double)m[0][1] * (m[1][0] * m[2][2] - m[1][2] * m[2][0]) +
            (double)m[0][2] * (m[1][0] * m[2][1] - m[1][1] * m[2][0]);
        CHECK(fabs(det) > 1e-6, "cam_xyz is invertible (det %.4f)", det);
    }
    CHECK(cd.wbct_rows >= 0 && cd.wbct_rows <= 64, "wbct row count in range");
    for (int i = 0; i < cd.wbct_rows; i++) {
        CHECK(cd.wbct[i][0] > 1000.0f && cd.wbct[i][0] < 30000.0f,
              "wbct row %d is a plausible temperature (%.0f K)", i,
              cd.wbct[i][0]);
        CHECK(cd.wbct[i][2] > 0.0f, "wbct row %d has a green multiplier", i);
    }
    /* The terminator search must not leave a populated row past the count. */
    if (cd.wbct_rows < 64) {
        CHECK(cd.wbct[cd.wbct_rows][0] == 0.0f,
              "the row after the last is empty");
    }
    if (cd.wbct_rows > 0) {
        printf("  wb table: %d rows, %.0f-%.0f K\n", cd.wbct_rows,
               cd.wbct[cd.wbct_rows - 1][0], cd.wbct[0][0]);
    } else {
        printf("  wb table: none; colorimetric route only\n");
    }

    /* Metadata after a preview read must still work: the handle rewinds. */
    ria_metadata again;
    CHECK_OK(ria_raw_metadata(raw, &again));
    CHECK(again.width == meta.width, "metadata is stable across calls");

    ria_decode_options opt;
    ria_decode_options_defaults(&opt);
    opt.half_size = 1;
    ria_image* img = NULL;
    rc = ria_raw_decode(raw, &opt, &img);
    CHECK(rc == RIA_OK, "half-size decode: %s", ria_status_string(rc));
    if (rc == RIA_OK) {
        CHECK(img->width > 0 && img->height > 0, "decoded dimensions");
        CHECK(img->data_size == img->stride * (size_t)img->height,
              "buffer size matches the geometry");
        CHECK(img->pending_flip == RIA_FLIP_NONE,
              "orientation is applied by default");
        /* Half size is a quarter of the area, within a pixel of rounding. */
        CHECK(abs(img->width * 2 - meta.width) <= 2,
              "half size is half the width: %d vs %d", img->width, meta.width);
        printf("  half decode %d x %d\n", img->width, img->height);
        ria_image_free(img);
    }

    /* The same handle, decoded again with different options: this is the
     * path that has to rewind and re-read the file. */
    opt.half_size = 1;
    opt.output_bits = 16;
    opt.alpha = 1;
    img = NULL;
    rc = ria_raw_decode(raw, &opt, &img);
    CHECK(rc == RIA_OK, "second decode on the same handle: %s",
          ria_status_string(rc));
    if (rc == RIA_OK) {
        CHECK(img->bits == 16, "16-bit output requested and delivered");
        CHECK(img->channels == 4, "alpha requested and delivered");

        ria_adjustments adj;
        ria_adjustments_defaults(&adj);
        adj.contrast = 0.2f;
        adj.vibrance = 0.3f;
        CHECK_OK(ria_apply_adjustments(img, &adj));

        ria_image* thumb = NULL;
        CHECK_OK(ria_fit_within(img, 320, 320, RIA_RESIZE_TRIANGLE, &thumb));
        CHECK(thumb->width <= 320 && thumb->height <= 320, "thumbnail fits");
        CHECK(thumb->bits == 16, "resize preserves depth");
        ria_image_free(thumb);
        ria_image_free(img);
    }

    /* Unrotated output must hand back the rotation instead of applying it. */
    ria_decode_options_defaults(&opt);
    opt.half_size = 1;
    opt.apply_orientation = 0;
    img = NULL;
    rc = ria_raw_decode(raw, &opt, &img);
    if (rc == RIA_OK) {
        CHECK(img->pending_flip == meta.flip,
              "unrotated decode reports the pending flip: %d vs %d",
              img->pending_flip, meta.flip);
        ria_image_free(img);
    }

    /* ── Phase A: the scene-referred path ───────────────────────────────── */

    ria_decode_options lin;
    ria_decode_options_scene_linear(&lin);
    lin.half_size = 1;
    ria_image* scene = NULL;
    rc = ria_raw_decode(raw, &lin, &scene);
    CHECK(rc == RIA_OK, "linear decode: %s", ria_status_string(rc));
    if (rc == RIA_OK) {
        CHECK(scene->transfer == RIA_TRANSFER_LINEAR,
              "the linear preset labels its output scene-referred");
        CHECK(scene->bits == 16, "and decodes at 16 bits");

        /*
         * The decode must leave headroom to work in. A linear,
         * un-auto-brightened decode should sit well below saturation — if
         * this starts failing, either the preset regressed or LibRaw changed
         * its normalisation, and every EV operation downstream is affected.
         */
        size_t clipped = 0;
        const size_t n = (size_t)scene->width * scene->height;
        const uint16_t* p = (const uint16_t*)scene->data;
        for (size_t i = 0; i < n; i++) {
            const uint16_t* q = p + i * scene->channels;
            if (q[0] == 65535 || q[1] == 65535 || q[2] == 65535) clipped++;
        }
        const double clip_pct = 100.0 * (double)clipped / (double)n;
        CHECK(clip_pct < 0.5, "linear decode clips %.3f%%, expected under 0.5%%",
              clip_pct);
        printf("  linear decode clips %.3f%%\n", clip_pct);

        /*
         * The load-bearing test for RIA_DISPLAY_CLIP: rendering the linear
         * decode through the default display transform must reproduce what
         * LibRaw produces when it applies the same curve itself. If these
         * diverge, the reimplementation of dcraw's gamma_curve is wrong and
         * every scene-referred render is subtly off.
         */
        ria_decode_options direct = lin;
        direct.gamma_power = 2.222f;
        direct.gamma_slope = 4.5f;
        direct.output_bits = 8;
        ria_image* reference = NULL;
        if (ria_raw_decode(raw, &direct, &reference) == RIA_OK) {
            ria_image* rendered = NULL;
            CHECK_OK(ria_apply_display_transform(scene, NULL, RIA_FMT_RGB8,
                                                 &rendered));
            if (rendered && rendered->data_size == reference->data_size) {
                double sum = 0.0;
                int worst = 0;
                for (size_t i = 0; i < reference->data_size; i++) {
                    const int d = abs((int)rendered->data[i] -
                                      (int)reference->data[i]);
                    sum += d;
                    if (d > worst) worst = d;
                }
                const double mean = sum / (double)reference->data_size;
                CHECK(mean < 1.0,
                      "linear + display transform matches a gamma decode "
                      "(mean diff %.3f, worst %d)", mean, worst);
                printf("  display transform vs LibRaw gamma: mean %.3f, "
                       "worst %d\n", mean, worst);
            }
            ria_image_free(rendered);
            ria_image_free(reference);
        }

        /* Exposure, done correctly, must brighten without the encoded-domain
         * blowout: half the grey point is one stop. */
        ria_display_transform dt;
        ria_display_transform_defaults(&dt);
        dt.grey_point = 0.09f;
        dt.mode = RIA_DISPLAY_SHOULDER;
        dt.white_point = 2.0f;
        ria_image* lifted = NULL;
        CHECK_OK(ria_apply_display_transform(scene, &dt, RIA_FMT_RGB8, &lifted));
        if (lifted) {
            size_t white = 0;
            for (size_t i = 0; i < lifted->data_size; i++) {
                if (lifted->data[i] == 255) white++;
            }
            CHECK(100.0 * white / lifted->data_size < 5.0,
                  "+1 EV with a shoulder does not blow out: %.2f%% at white",
                  100.0 * white / lifted->data_size);
            ria_image_free(lifted);
        }

        ria_image_free(scene);
    }

    ria_image* sensor = NULL;
    uint32_t filters = 0;
    int black = 0, white = 0;
    rc = ria_raw_sensor_image(raw, &sensor, &filters, &black, &white);
    CHECK(rc == RIA_OK || rc == RIA_ERR_UNSUPPORTED, "sensor image: %s",
          ria_status_string(rc));
    if (rc == RIA_OK) {
        CHECK(sensor->bits == 16 && sensor->channels == 1,
              "the mosaic is single-channel 16-bit");
        CHECK(sensor->width >= meta.raw_width, "mosaic covers the active area");
        CHECK(white > black, "levels are ordered: %d..%d", black, white);
        printf("  mosaic %d x %d, filters 0x%08x, levels %d..%d\n",
               sensor->width, sensor->height, filters, black, white);
        ria_image_free(sensor);
    }

    ria_raw_close(raw);
}

static void test_missing_file(void) {
    section("error handling");

    ria_raw* raw = NULL;
    ria_status rc = ria_raw_open("/definitely/not/here.NEF", &raw);
    CHECK(rc != RIA_OK, "a missing file fails");
    CHECK(raw == NULL, "the handle is NULL after a failed open");

    ria_metadata meta;
    CHECK(ria_read_metadata("/definitely/not/here.NEF", &meta) != RIA_OK,
          "metadata of a missing file fails");
    CHECK(ria_raw_open(NULL, &raw) == RIA_ERR_INVALID, "NULL path is rejected");

    ria_image* img = NULL;
    CHECK(ria_image_convert(NULL, RIA_FMT_RGB8, &img) == RIA_ERR_INVALID,
          "NULL source is rejected");
    CHECK(ria_gaussian_blur(NULL, 1.0f) == RIA_ERR_INVALID,
          "NULL image is rejected");
    ria_raw_close(NULL); /* must not crash */
}

int main(int argc, char** argv) {
    printf("raw_images_api %s, LibRaw %s\n\n", ria_version_string(),
           ria_libraw_version_string());

    test_basics();
    test_image_lifecycle();
    test_convert();
    test_geometry();
    test_adjustments();
    test_transfer();
    test_display_transform();
    test_encoding_propagation();
    test_statistics();
    test_filters();
    test_focus_math();
    test_pnm();
    test_missing_file();

    test_color_data_arguments();

    for (int i = 1; i < argc; i++) test_raw_file(argv[i]);

    printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
