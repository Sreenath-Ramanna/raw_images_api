/*
 * ria_focus.c — where the camera focused.
 *
 * Vendors record this in MakerNote blobs that LibRaw hands back verbatim.
 * Parsing them is cheap (the blobs are read during open, so no unpack is
 * needed) but the coordinate systems are inconsistent, so parsing and
 * interpretation are kept apart: ria_raw_focus returns the vendor's own
 * numbers, ria_focus_resolve turns them into pixels.
 */

#include "ria_internal.h"

#include <libraw/libraw.h>

/* Documented as process-wide in the header: it is a per-camera-family
 * correction, not per-image state, and threading it through every call would
 * put a rarely-used knob in the hot path of a common one. */
static int g_canon_y_up = 1;

void ria_focus_set_canon_y_up(int y_is_up) { g_canon_y_up = y_is_up ? 1 : 0; }
int ria_focus_get_canon_y_up(void) { return g_canon_y_up; }

/* MakerNote AF blobs are little-endian regardless of the file's own byte
 * order, because LibRaw has already normalised the container. */
static uint16_t af_u16(const unsigned char* p) {
    return (uint16_t)(p[0] | (p[1] << 8));
}

static int16_t af_s16(const unsigned char* p) {
    return (int16_t)(p[0] | (p[1] << 8));
}

/*
 * Canon AFInfo2 (MakerNote tag 0x0026): a flat int16 array — a fixed header,
 * then four NumAFPoints-long arrays (widths, heights, xs, ys), then bitmasks
 * flagging which points reported focus.
 */
static void parse_canon_af(const unsigned char* d, unsigned len,
                           ria_focus_point* out) {
    if (len < 16) return;

    const unsigned num = af_u16(d + 4);
    out->af_image_width = af_u16(d + 12);
    out->af_image_height = af_u16(d + 14);
    if (num == 0) return;

    const unsigned need = 16 + num * 8;          /* widths+heights+xs+ys */
    const unsigned mask_words = (num + 15) / 16;
    if (need + mask_words * 2 > len) return;

    const unsigned char* widths = d + 16;
    const unsigned char* heights = widths + num * 2;
    const unsigned char* xs = heights + num * 2;
    const unsigned char* ys = xs + num * 2;
    const unsigned char* in_focus = ys + num * 2;

    /* Average the points that reported focus. With subject tracking there is
     * usually exactly one; averaging keeps a multi-point result centred
     * rather than arbitrarily picking the first. */
    long sum_x = 0, sum_y = 0, sum_w = 0, sum_h = 0;
    int count = 0;
    for (unsigned i = 0; i < num; i++) {
        if (!(af_u16(in_focus + (i / 16) * 2) & (1u << (i % 16)))) continue;
        sum_x += af_s16(xs + i * 2);
        sum_y += af_s16(ys + i * 2);
        sum_w += af_u16(widths + i * 2);
        sum_h += af_u16(heights + i * 2);
        count++;
    }
    if (count == 0) return;

    out->x = (int)(sum_x / count);
    out->y = (int)(sum_y / count);
    out->width = (int)(sum_w / count);
    out->height = (int)(sum_h / count);
    out->points_in_focus = count;
    out->valid = 1;
}

/*
 * Nikon AFInfo2 (MakerNote tag 0x00b7). The offsets are into the blob as
 * LibRaw presents it, which excludes the 4-byte version header that ExifTool
 * counts — subtract 4 from any offset quoted in ExifTool's documentation.
 * Verified against Z 6_2 files.
 */
static void parse_nikon_af(const unsigned char* d, unsigned len,
                           ria_focus_point* out) {
    if (len < 50) return;

    out->af_image_width = af_u16(d + 38);
    out->af_image_height = af_u16(d + 40);
    out->x = af_u16(d + 42);
    out->y = af_u16(d + 44);
    out->width = af_u16(d + 46);
    out->height = af_u16(d + 48);

    if (out->af_image_width == 0 || out->af_image_height == 0) return;
    /* A zeroed position means "no AF data recorded" rather than a subject in
     * the extreme top-left corner. */
    if (out->x == 0 && out->y == 0) return;

    out->points_in_focus = 1;
    out->valid = 1;
}

ria_status ria_raw_focus(ria_raw* raw, ria_focus_point* out) {
    if (!raw || !out) return RIA_ERR_INVALID;

    libraw_data_t* lr = (libraw_data_t*)ria_raw_native_handle(raw);
    if (!lr) return RIA_ERR_INVALID;

    memset(out, 0, sizeof(*out));
    out->flip = lr->sizes.flip;

    for (int i = 0; i < lr->makernotes.common.afcount &&
                    i < LIBRAW_AFDATA_MAXCOUNT; i++) {
        libraw_afinfo_item_t* item = &lr->makernotes.common.afdata[i];
        if (!item->AFInfoData || item->AFInfoData_length == 0) continue;

        if (item->AFInfoData_tag == 0x0026) {
            out->vendor = RIA_AF_VENDOR_CANON;
            parse_canon_af(item->AFInfoData,
                           (unsigned)item->AFInfoData_length, out);
        } else if (item->AFInfoData_tag == 0x00b7) {
            out->vendor = RIA_AF_VENDOR_NIKON;
            parse_nikon_af(item->AFInfoData,
                           (unsigned)item->AFInfoData_length, out);
        }
        if (out->valid) break;
    }

    /* Not an error: manual focus, adapted lenses and unsupported bodies all
     * legitimately produce no AF record. */
    return out->valid ? RIA_OK : RIA_ERR_NO_DATA;
}

ria_status ria_focus_resolve(const ria_focus_point* focus, int image_width,
                             int image_height, ria_focus_area* out) {
    if (!focus || !out) return RIA_ERR_INVALID;
    if (!focus->valid) return RIA_ERR_NO_DATA;
    if (focus->af_image_width <= 0 || focus->af_image_height <= 0) {
        return RIA_ERR_NO_DATA;
    }
    if (image_width <= 0 || image_height <= 0) return RIA_ERR_INVALID;

    /* 1. Vendor coordinates -> unrotated AF space, origin top-left. */
    double x_af, y_af;
    if (focus->vendor == RIA_AF_VENDOR_CANON) {
        /* Origin at the image centre, signed, positive x to the right. */
        x_af = focus->x + focus->af_image_width / 2.0;
        y_af = g_canon_y_up ? focus->af_image_height / 2.0 - focus->y
                            : focus->af_image_height / 2.0 + focus->y;
    } else {
        /* Nikon: already top-left and unsigned. */
        x_af = focus->x;
        y_af = focus->y;
    }

    /* 2. Scale into the decoded image. AF space matches the embedded preview,
     *    which is slightly smaller than the full decode, so this is not a
     *    no-op. image_width/height are post-rotation; undo that first. */
    const int quarter = (focus->flip == RIA_FLIP_90_CCW ||
                         focus->flip == RIA_FLIP_90_CW);
    const double unrotated_w = quarter ? image_height : image_width;
    const double unrotated_h = quarter ? image_width : image_height;

    const double sx = unrotated_w / focus->af_image_width;
    const double sy = unrotated_h / focus->af_image_height;

    const double px = x_af * sx;
    const double py = y_af * sy;
    const double w = focus->width * sx;
    const double h = focus->height * sy;

    /* 3. Rotate to match the decoded image. Mirrors ria_apply_orientation, so
     *    a marker drawn here lands on the pixels it describes. */
    switch (focus->flip) {
        case RIA_FLIP_90_CCW:
            out->center_x = py;
            out->center_y = unrotated_w - px;
            out->width = h;
            out->height = w;
            break;
        case RIA_FLIP_90_CW:
            out->center_x = unrotated_h - py;
            out->center_y = px;
            out->width = h;
            out->height = w;
            break;
        case RIA_FLIP_180:
            out->center_x = unrotated_w - px;
            out->center_y = unrotated_h - py;
            out->width = w;
            out->height = h;
            break;
        default:
            out->center_x = px;
            out->center_y = py;
            out->width = w;
            out->height = h;
            break;
    }
    return RIA_OK;
}
