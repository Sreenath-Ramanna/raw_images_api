/*
 * ria_tool.c — a command-line exercise of the whole API.
 *
 *   ria_tool info    <raw>
 *   ria_tool preview <raw> <out.jpg|out.ppm>
 *   ria_tool decode  <raw> <out.ppm> [options]
 *   ria_tool bench   <raw>
 *
 * Doubles as the worked example for the documentation: every call the
 * library exposes for reading a file appears here once.
 */

#include "raw_images_api.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static double now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1.0e6;
}

static int fail(const char* what, ria_status rc) {
    fprintf(stderr, "%s: %s\n", what, ria_status_string(rc));
    return 1;
}

static const char* flip_name(int flip) {
    switch (flip) {
        case RIA_FLIP_NONE:    return "upright";
        case RIA_FLIP_180:     return "180 degrees";
        case RIA_FLIP_90_CCW:  return "90 CCW";
        case RIA_FLIP_90_CW:   return "90 CW";
        default:               return "unknown";
    }
}

/* ── info ────────────────────────────────────────────────────────────────── */

static int cmd_info(const char* path) {
    ria_raw* raw = NULL;
    ria_status rc = ria_raw_open(path, &raw);
    if (rc != RIA_OK) return fail(path, rc);

    ria_metadata m;
    rc = ria_raw_metadata(raw, &m);
    if (rc != RIA_OK) {
        ria_raw_close(raw);
        return fail("metadata", rc);
    }

    printf("file        %s\n", path);
    printf("camera      %s %s\n", m.make, m.model);
    if (m.lens[0]) printf("lens        %s\n", m.lens);
    printf("displayed   %d x %d (%s)\n", m.width, m.height, flip_name(m.flip));
    printf("sensor      %d x %d, %d colours\n", m.raw_width, m.raw_height,
           m.colors);
    printf("exposure    ISO %.0f  ", (double)m.iso_speed);
    if (m.shutter > 0.0f && m.shutter < 1.0f) {
        printf("1/%.0fs  ", 1.0 / m.shutter);
    } else {
        printf("%.1fs  ", (double)m.shutter);
    }
    printf("f/%.1f  %.0fmm\n", (double)m.aperture, (double)m.focal_len);
    printf("levels      black %d, white %d\n", m.black_level, m.white_level);
    printf("camera wb   %.3f %.3f %.3f %.3f\n", (double)m.cam_mul[0],
           (double)m.cam_mul[1], (double)m.cam_mul[2], (double)m.cam_mul[3]);
    if (m.timestamp) {
        char buf[64];
        time_t t = (time_t)m.timestamp;
        struct tm tm;
        localtime_r(&t, &tm);
        strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tm);
        printf("taken       %s\n", buf);
    }

    ria_focus_point focus;
    rc = ria_raw_focus(raw, &focus);
    if (rc == RIA_OK) {
        ria_focus_area area;
        if (ria_focus_resolve(&focus, m.width, m.height, &area) == RIA_OK) {
            printf("focus       %s, %d point(s), centre (%.0f, %.0f), "
                   "area %.0f x %.0f\n",
                   focus.vendor == RIA_AF_VENDOR_CANON ? "Canon" : "Nikon",
                   focus.points_in_focus, area.center_x, area.center_y,
                   area.width, area.height);
        }
    } else {
        printf("focus       none recorded\n");
    }

    ria_preview* preview = NULL;
    if (ria_raw_preview(raw, &preview) == RIA_OK) {
        printf("preview     %d x %d, %s, %zu bytes\n", preview->width,
               preview->height,
               preview->format == RIA_PREVIEW_JPEG ? "JPEG" : "bitmap",
               preview->data_size);
        ria_preview_free(preview);
    }

    ria_raw_close(raw);
    return 0;
}

/* ── preview ─────────────────────────────────────────────────────────────── */

static int cmd_preview(const char* path, const char* out_path) {
    ria_preview* preview = NULL;
    ria_status rc = ria_extract_preview(path, &preview);
    if (rc != RIA_OK) return fail("preview", rc);

    int result = 0;
    if (preview->format == RIA_PREVIEW_JPEG) {
        /* The bytes are the camera's own JPEG; write them as they are rather
         * than decoding and re-encoding. */
        FILE* f = fopen(out_path, "wb");
        if (!f || fwrite(preview->data, 1, preview->data_size, f) !=
                      preview->data_size) {
            fprintf(stderr, "could not write %s\n", out_path);
            result = 1;
        }
        if (f) fclose(f);
        printf("wrote %s (%zu bytes, rotate %s to display)\n", out_path,
               preview->data_size, flip_name(preview->flip));
    } else {
        ria_image* img = NULL;
        rc = ria_preview_to_image(preview, RIA_FMT_RGB8, &img);
        if (rc != RIA_OK) {
            ria_preview_free(preview);
            return fail("preview decode", rc);
        }
        ria_image* upright = NULL;
        rc = ria_apply_orientation(img, img->pending_flip, &upright);
        ria_image_free(img);
        if (rc != RIA_OK) {
            ria_preview_free(preview);
            return fail("orientation", rc);
        }
        rc = ria_write_pnm(upright, out_path);
        ria_image_free(upright);
        if (rc != RIA_OK) result = fail("write", rc);
    }

    ria_preview_free(preview);
    return result;
}

/* ── decode ──────────────────────────────────────────────────────────────── */

typedef struct {
    ria_decode_options decode;
    ria_adjustments    adjust;
    float              auto_levels;   /* < 0 = off                          */
    float              sharpen;       /* amount, 0 = off                    */
    float              blur;          /* sigma, 0 = off                     */
    int                fit;           /* longest edge, 0 = off              */
} tool_options;

static void usage(void) {
    fprintf(stderr,
        "usage: ria_tool <command> [args]\n"
        "\n"
        "  info    <raw>                    metadata, focus point, preview\n"
        "  preview <raw> <out>              extract the embedded preview\n"
        "  decode  <raw> <out.ppm> [opts]   full decode, optionally enhanced\n"
        "  bench   <raw>                    time each stage\n"
        "\n"
        "decode options:\n"
        "  --half                 quarter-area decode, roughly 4x faster\n"
        "  --bits 8|16            output depth (default 8)\n"
        "  --demosaic N           0 linear 1 VNG 2 PPG 3 AHD 4 DCB 11 DHT\n"
        "  --no-auto-bright       do not stretch the histogram\n"
        "  --exposure EV          -5..5\n"
        "  --contrast N           -1..1\n"
        "  --shadows N            -1..1\n"
        "  --highlights N         -1..1\n"
        "  --saturation N         -1..2\n"
        "  --vibrance N           -1..2\n"
        "  --gamma N              >0\n"
        "  --auto-levels [pct]    stretch each channel, default clip 0.5%%\n"
        "  --sharpen N            unsharp mask amount, sigma 1.0\n"
        "  --blur N               gaussian sigma\n"
        "  --fit N                fit the longest edge to N pixels\n");
}

static int parse_decode_args(int argc, char** argv, tool_options* o) {
    ria_decode_options_defaults(&o->decode);
    ria_adjustments_defaults(&o->adjust);
    o->auto_levels = -1.0f;
    o->sharpen = 0.0f;
    o->blur = 0.0f;
    o->fit = 0;

    for (int i = 0; i < argc; i++) {
        const char* a = argv[i];
        const char* next = (i + 1 < argc) ? argv[i + 1] : NULL;

#define NEED_VALUE()                                                    \
        do {                                                            \
            if (!next) {                                                \
                fprintf(stderr, "%s needs a value\n", a);               \
                return -1;                                              \
            }                                                           \
            i++;                                                        \
        } while (0)

        if (strcmp(a, "--half") == 0) {
            o->decode.half_size = 1;
        } else if (strcmp(a, "--no-auto-bright") == 0) {
            o->decode.no_auto_bright = 1;
        } else if (strcmp(a, "--bits") == 0) {
            NEED_VALUE();
            o->decode.output_bits = atoi(next);
        } else if (strcmp(a, "--demosaic") == 0) {
            NEED_VALUE();
            o->decode.demosaic = (ria_demosaic)atoi(next);
        } else if (strcmp(a, "--exposure") == 0) {
            NEED_VALUE();
            o->adjust.exposure_ev = (float)atof(next);
        } else if (strcmp(a, "--contrast") == 0) {
            NEED_VALUE();
            o->adjust.contrast = (float)atof(next);
        } else if (strcmp(a, "--shadows") == 0) {
            NEED_VALUE();
            o->adjust.shadows = (float)atof(next);
        } else if (strcmp(a, "--highlights") == 0) {
            NEED_VALUE();
            o->adjust.highlights = (float)atof(next);
        } else if (strcmp(a, "--saturation") == 0) {
            NEED_VALUE();
            o->adjust.saturation = (float)atof(next);
        } else if (strcmp(a, "--vibrance") == 0) {
            NEED_VALUE();
            o->adjust.vibrance = (float)atof(next);
        } else if (strcmp(a, "--gamma") == 0) {
            NEED_VALUE();
            o->adjust.gamma = (float)atof(next);
        } else if (strcmp(a, "--auto-levels") == 0) {
            /* The percentage is optional, so only consume a following
             * argument when it actually looks like a number. */
            if (next && next[0] != '-') {
                o->auto_levels = (float)atof(next);
                i++;
            } else {
                o->auto_levels = 0.5f;
            }
        } else if (strcmp(a, "--sharpen") == 0) {
            NEED_VALUE();
            o->sharpen = (float)atof(next);
        } else if (strcmp(a, "--blur") == 0) {
            NEED_VALUE();
            o->blur = (float)atof(next);
        } else if (strcmp(a, "--fit") == 0) {
            NEED_VALUE();
            o->fit = atoi(next);
        } else {
            fprintf(stderr, "unknown option %s\n", a);
            return -1;
        }
#undef NEED_VALUE
    }
    return 0;
}

static int cmd_decode(const char* path, const char* out_path, int argc,
                      char** argv) {
    tool_options o;
    if (parse_decode_args(argc, argv, &o) != 0) return 1;

    double t0 = now_ms();
    ria_image* img = NULL;
    ria_status rc = ria_decode_file(path, &o.decode, &img);
    if (rc != RIA_OK) return fail("decode", rc);
    printf("decode        %7.0f ms  %d x %d, %d-bit\n", now_ms() - t0,
           img->width, img->height, img->bits);

    if (o.auto_levels >= 0.0f) {
        t0 = now_ms();
        rc = ria_auto_levels(img, o.auto_levels);
        if (rc != RIA_OK) goto done;
        printf("auto levels   %7.0f ms\n", now_ms() - t0);
    }

    if (!ria_adjustments_is_identity(&o.adjust)) {
        t0 = now_ms();
        rc = ria_apply_adjustments(img, &o.adjust);
        if (rc != RIA_OK) goto done;
        printf("adjustments   %7.0f ms\n", now_ms() - t0);
    }

    if (o.blur > 0.0f) {
        t0 = now_ms();
        rc = ria_gaussian_blur(img, o.blur);
        if (rc != RIA_OK) goto done;
        printf("blur          %7.0f ms\n", now_ms() - t0);
    }

    if (o.sharpen > 0.0f) {
        t0 = now_ms();
        rc = ria_unsharp_mask(img, 1.0f, o.sharpen, 0.01f);
        if (rc != RIA_OK) goto done;
        printf("sharpen       %7.0f ms\n", now_ms() - t0);
    }

    if (o.fit > 0) {
        t0 = now_ms();
        ria_image* small = NULL;
        rc = ria_fit_within(img, o.fit, o.fit, RIA_RESIZE_TRIANGLE, &small);
        if (rc != RIA_OK) goto done;
        ria_image_free(img);
        img = small;
        printf("resize        %7.0f ms  %d x %d\n", now_ms() - t0, img->width,
               img->height);
    }

    t0 = now_ms();
    rc = ria_write_pnm(img, out_path);
    if (rc == RIA_OK) printf("write         %7.0f ms  %s\n", now_ms() - t0, out_path);

done:
    ria_image_free(img);
    return rc == RIA_OK ? 0 : fail("processing", rc);
}

/* ── bench ───────────────────────────────────────────────────────────────── */

static int cmd_bench(const char* path) {
    double t0 = now_ms();
    ria_raw* raw = NULL;
    ria_status rc = ria_raw_open(path, &raw);
    if (rc != RIA_OK) return fail("open", rc);
    printf("open          %7.1f ms\n", now_ms() - t0);

    ria_metadata m;
    t0 = now_ms();
    rc = ria_raw_metadata(raw, &m);
    printf("metadata      %7.1f ms\n", now_ms() - t0);

    ria_focus_point focus;
    t0 = now_ms();
    ria_raw_focus(raw, &focus);
    printf("focus         %7.1f ms\n", now_ms() - t0);

    ria_preview* preview = NULL;
    t0 = now_ms();
    rc = ria_raw_preview(raw, &preview);
    printf("preview       %7.1f ms  %s\n", now_ms() - t0,
           rc == RIA_OK ? "ok" : ria_status_string(rc));
    if (preview) ria_preview_free(preview);

    ria_decode_options opt;
    ria_decode_options_defaults(&opt);
    opt.half_size = 1;
    ria_image* img = NULL;
    t0 = now_ms();
    rc = ria_raw_decode(raw, &opt, &img);
    printf("decode half   %7.1f ms  %s\n", now_ms() - t0,
           rc == RIA_OK ? "ok" : ria_status_string(rc));
    if (img) ria_image_free(img);

    /* Again at full size on the same handle — this is the path that has to
     * rewind the file internally. */
    opt.half_size = 0;
    img = NULL;
    t0 = now_ms();
    rc = ria_raw_decode(raw, &opt, &img);
    printf("decode full   %7.1f ms  %s\n", now_ms() - t0,
           rc == RIA_OK ? "ok" : ria_status_string(rc));

    if (img) {
        ria_adjustments adj;
        ria_adjustments_defaults(&adj);
        adj.contrast = 0.15f;
        adj.vibrance = 0.2f;
        t0 = now_ms();
        ria_apply_adjustments(img, &adj);
        printf("adjustments   %7.1f ms\n", now_ms() - t0);

        ria_histogram hist;
        t0 = now_ms();
        ria_compute_histogram(img, &hist);
        printf("histogram     %7.1f ms  %.2f%% black, %.2f%% white clipped\n",
               now_ms() - t0, hist.clipped_black * 100.0,
               hist.clipped_white * 100.0);

        ria_image* thumb = NULL;
        t0 = now_ms();
        ria_fit_within(img, 800, 800, RIA_RESIZE_TRIANGLE, &thumb);
        printf("resize 800px  %7.1f ms\n", now_ms() - t0);
        ria_image_free(thumb);

        t0 = now_ms();
        ria_unsharp_mask(img, 1.0f, 0.6f, 0.01f);
        printf("unsharp       %7.1f ms\n", now_ms() - t0);

        ria_image_free(img);
    }

    ria_raw_close(raw);
    return 0;
}

/* ── main ────────────────────────────────────────────────────────────────── */

int main(int argc, char** argv) {
    if (argc < 3) {
        printf("raw_images_api %s (LibRaw %s)\n", ria_version_string(),
               ria_libraw_version_string());
        usage();
        return argc < 2 ? 0 : 1;
    }

    const char* cmd = argv[1];
    if (strcmp(cmd, "info") == 0) return cmd_info(argv[2]);
    if (strcmp(cmd, "bench") == 0) return cmd_bench(argv[2]);

    if (argc < 4) {
        usage();
        return 1;
    }
    if (strcmp(cmd, "preview") == 0) return cmd_preview(argv[2], argv[3]);
    if (strcmp(cmd, "decode") == 0) {
        return cmd_decode(argv[2], argv[3], argc - 4, argv + 4);
    }

    usage();
    return 1;
}
