/*
 * ria_raw.c — opening RAW files and getting pixels or facts out of them.
 *
 * LibRaw's own model is one object per file, mutated through a fixed
 * sequence: open -> unpack -> process -> make_mem_image. That sequence is
 * one-shot; the demosaic consumes the unpacked data. ria_raw hides this by
 * tracking how far along the handle is and rewinding transparently when a
 * caller asks for something the current state cannot deliver — which is what
 * makes "decode this again with different settings" work.
 */

#include "ria_internal.h"

#include <libraw/libraw.h>
#include <stdlib.h>
#include <string.h>

struct ria_raw {
    libraw_data_t* lr;

    /* Enough to reopen the same source after a decode has consumed it. */
    char*          path;        /* NULL for a buffer source                  */
    const void*    buffer;      /* not owned; caller guarantees lifetime     */
    size_t         buffer_size;

    int            headers_ok;  /* open succeeded and metadata is parsed     */
    int            consumed;    /* a decode has eaten the unpacked data      */
    int            thumb_ready;
};

/* ── Error mapping ───────────────────────────────────────────────────────── */

static ria_status map_libraw_error(int err) {
    if (err == LIBRAW_SUCCESS) return RIA_OK;
    /* Positive values are a system errno passed straight through. */
    if (err > 0) return RIA_ERR_IO;

    switch (err) {
        case LIBRAW_FILE_UNSUPPORTED:
        case LIBRAW_UNSUPPORTED_THUMBNAIL:      return RIA_ERR_UNSUPPORTED;
        case LIBRAW_NO_THUMBNAIL:
        case LIBRAW_REQUEST_FOR_NONEXISTENT_IMAGE: return RIA_ERR_NO_DATA;
        case LIBRAW_UNSUFFICIENT_MEMORY:
        case LIBRAW_MEMPOOL_OVERFLOW:           return RIA_ERR_MEMORY;
        case LIBRAW_IO_ERROR:
        case LIBRAW_INPUT_CLOSED:               return RIA_ERR_IO;
        case LIBRAW_DATA_ERROR:
        case LIBRAW_TOO_BIG:
        case LIBRAW_BAD_CROP:                   return RIA_ERR_DECODE;
        case LIBRAW_OUT_OF_ORDER_CALL:          return RIA_ERR_INTERNAL;
        default:                                return RIA_ERR_DECODE;
    }
}

/* ── Open / close ────────────────────────────────────────────────────────── */

static ria_status open_source(ria_raw* raw) {
    int err;
    if (raw->path) {
        err = libraw_open_file(raw->lr, raw->path);
    } else {
        err = libraw_open_buffer(raw->lr, raw->buffer, raw->buffer_size);
    }
    if (err != LIBRAW_SUCCESS) {
        raw->headers_ok = 0;
        return map_libraw_error(err);
    }
    raw->headers_ok = 1;
    raw->consumed = 0;
    raw->thumb_ready = 0;
    return RIA_OK;
}

/* Return the handle to a state where headers are valid and the sensor data
 * has not yet been consumed. Cheap when nothing has been decoded yet. */
static ria_status rewind_if_needed(ria_raw* raw) {
    if (raw->headers_ok && !raw->consumed) return RIA_OK;
    libraw_recycle(raw->lr);
    return open_source(raw);
}

/* An empty handle with a live LibRaw object, before a source is attached. */
static ria_raw* raw_create(void) {
    ria_raw* raw = (ria_raw*)calloc(1, sizeof(*raw));
    if (!raw) return NULL;

    raw->lr = libraw_init(0);
    if (!raw->lr) {
        free(raw);
        return NULL;
    }
    return raw;
}

ria_status ria_raw_open(const char* path, ria_raw** out) {
    if (!path || !out) return RIA_ERR_INVALID;
    *out = NULL;

    ria_raw* raw = raw_create();
    if (!raw) return RIA_ERR_MEMORY;

    raw->path = strdup(path);
    if (!raw->path) {
        ria_raw_close(raw);
        return RIA_ERR_MEMORY;
    }

    ria_status rc = open_source(raw);
    if (rc != RIA_OK) {
        ria_raw_close(raw);
        return rc;
    }

    *out = raw;
    return RIA_OK;
}

ria_status ria_raw_open_buffer(const void* data, size_t size, ria_raw** out) {
    if (!data || size == 0 || !out) return RIA_ERR_INVALID;
    *out = NULL;

    ria_raw* raw = raw_create();
    if (!raw) return RIA_ERR_MEMORY;

    raw->buffer = data;
    raw->buffer_size = size;

    ria_status rc = open_source(raw);
    if (rc != RIA_OK) {
        ria_raw_close(raw);
        return rc;
    }

    *out = raw;
    return RIA_OK;
}

void ria_raw_close(ria_raw* raw) {
    if (!raw) return;
    if (raw->lr) libraw_close(raw->lr);
    free(raw->path);
    free(raw);
}

const char* ria_raw_path(const ria_raw* raw) {
    if (!raw) return "";
    return raw->path ? raw->path : "";
}

void* ria_raw_native_handle(ria_raw* raw) { return raw ? raw->lr : NULL; }

/* ── Metadata ────────────────────────────────────────────────────────────── */

static void copy_str(char* dst, size_t dst_size, const char* src) {
    if (!src) {
        dst[0] = '\0';
        return;
    }
    /* strnlen + memcpy rather than strncpy: the source fields are fixed-size
     * char arrays that LibRaw does not guarantee to terminate, and strncpy
     * would either read past the end or leave the result unterminated. */
    const size_t n = strnlen(src, dst_size - 1);
    memcpy(dst, src, n);
    dst[n] = '\0';
}

ria_status ria_raw_metadata(ria_raw* raw, ria_metadata* out) {
    if (!raw || !out) return RIA_ERR_INVALID;
    ria_status rc = rewind_if_needed(raw);
    if (rc != RIA_OK) return rc;

    memset(out, 0, sizeof(*out));
    libraw_data_t* lr = raw->lr;

    copy_str(out->make, sizeof(out->make), lr->idata.make);
    copy_str(out->model, sizeof(out->model), lr->idata.model);
    copy_str(out->software, sizeof(out->software), lr->idata.software);
    copy_str(out->artist, sizeof(out->artist), lr->other.artist);

    /* EXIF carries the lens name in one place and the MakerNote in another,
     * and which one is populated varies by body. Prefer the EXIF field and
     * fall back rather than reporting an empty string. */
    if (lr->lens.Lens[0]) {
        copy_str(out->lens, sizeof(out->lens), lr->lens.Lens);
    } else {
        copy_str(out->lens, sizeof(out->lens), lr->lens.makernotes.Lens);
    }

    out->iso_speed = lr->other.iso_speed;
    out->shutter = lr->other.shutter;
    out->aperture = lr->other.aperture;
    out->focal_len = lr->other.focal_len;
    out->timestamp = (int64_t)lr->other.timestamp;

    out->flip = lr->sizes.flip;
    out->raw_width = lr->sizes.width;
    out->raw_height = lr->sizes.height;

    /*
     * sizes.width/height describe the unrotated sensor area, but a decode
     * bakes the camera orientation in — so a portrait frame comes out
     * transposed. Report what will actually be displayed, otherwise every
     * portrait shot claims to be landscape.
     */
    if (out->flip == RIA_FLIP_90_CCW || out->flip == RIA_FLIP_90_CW) {
        out->width = lr->sizes.height;
        out->height = lr->sizes.width;
    } else {
        out->width = lr->sizes.width;
        out->height = lr->sizes.height;
    }

    out->colors = lr->idata.colors;
    out->black_level = (int)lr->color.black;
    out->white_level = (int)lr->color.maximum;
    for (int i = 0; i < 4; i++) out->cam_mul[i] = lr->color.cam_mul[i];

    return RIA_OK;
}

ria_status ria_raw_color_data(ria_raw* raw, ria_color_data* out) {
    if (!raw || !out) return RIA_ERR_INVALID;
    ria_status rc = rewind_if_needed(raw);
    if (rc != RIA_OK) return rc;

    memset(out, 0, sizeof(*out));
    libraw_data_t* lr = raw->lr;

    for (int i = 0; i < 4; i++) {
        out->cam_mul[i] = lr->color.cam_mul[i];
        out->pre_mul[i] = lr->color.pre_mul[i];
        for (int j = 0; j < 3; j++) out->cam_xyz[i][j] = lr->color.cam_xyz[i][j];
    }
    out->colors = lr->idata.colors;

    /*
     * WBCT_Coeffs is a fixed 64-row array with no count beside it; LibRaw
     * leaves the unused tail zeroed, so a zero Kelvin entry is the terminator.
     * Stopping at the first one rather than trusting the whole array is what
     * keeps a caller from interpolating through rows of zeroes.
     */
    for (int i = 0; i < 64; i++) {
        if (lr->color.WBCT_Coeffs[i][0] <= 0.0f) break;
        for (int j = 0; j < 5; j++) {
            out->wbct[i][j] = lr->color.WBCT_Coeffs[i][j];
        }
        out->wbct_rows = i + 1;
    }

    return RIA_OK;
}

/* ── Embedded preview ────────────────────────────────────────────────────── */

ria_status ria_raw_preview(ria_raw* raw, ria_preview** out) {
    if (!raw || !out) return RIA_ERR_INVALID;
    *out = NULL;

    ria_status rc = rewind_if_needed(raw);
    if (rc != RIA_OK) return rc;

    if (!raw->thumb_ready) {
        int err = libraw_unpack_thumb(raw->lr);
        if (err != LIBRAW_SUCCESS) return map_libraw_error(err);
        raw->thumb_ready = 1;
    }

    int format;
    switch (raw->lr->thumbnail.tformat) {
        case LIBRAW_THUMBNAIL_JPEG:   format = RIA_PREVIEW_JPEG;   break;
        case LIBRAW_THUMBNAIL_BITMAP: format = RIA_PREVIEW_BITMAP; break;
        default:
            /* 16-bit, layered and video previews exist but are rare enough
             * that supporting them would be untested code; a caller that
             * needs pixels can always fall back to a decode. */
            return RIA_ERR_UNSUPPORTED;
    }

    if (!raw->lr->thumbnail.thumb || raw->lr->thumbnail.tlength == 0) {
        return RIA_ERR_NO_DATA;
    }

    ria_preview* p = (ria_preview*)calloc(1, sizeof(*p));
    if (!p) return RIA_ERR_MEMORY;

    p->data_size = (size_t)raw->lr->thumbnail.tlength;
    p->data = (uint8_t*)malloc(p->data_size);
    if (!p->data) {
        free(p);
        return RIA_ERR_MEMORY;
    }
    memcpy(p->data, raw->lr->thumbnail.thumb, p->data_size);

    p->format = format;
    p->width = raw->lr->thumbnail.twidth;
    p->height = raw->lr->thumbnail.theight;
    /* Previews are stored unrotated. Handing back the code rather than
     * applying it keeps this a memcpy; the consumer usually has a cheaper
     * place to rotate (a texture transform, a canvas). */
    p->flip = raw->lr->sizes.flip;

    *out = p;
    return RIA_OK;
}

void ria_preview_free(ria_preview* preview) {
    if (!preview) return;
    free(preview->data);
    free(preview);
}

int ria_has_jpeg_support(void) {
#ifdef RIA_WITH_JPEG
    return 1;
#else
    return 0;
#endif
}

ria_status ria_preview_to_image(const ria_preview* preview,
                                ria_pixel_format fmt, ria_image** out) {
    if (!preview || !preview->data || !out) return RIA_ERR_INVALID;
    *out = NULL;

    if (preview->format == RIA_PREVIEW_BITMAP) {
        if (preview->width <= 0 || preview->height <= 0) return RIA_ERR_NO_DATA;
        const size_t need =
            (size_t)preview->width * (size_t)preview->height * 3u;
        if (preview->data_size < need) return RIA_ERR_DECODE;

        ria_image* wrapped = NULL;
        ria_status rc = ria_image_wrap(preview->data, preview->width,
                                       preview->height, RIA_FMT_RGB8, &wrapped);
        if (rc != RIA_OK) return rc;
        rc = ria_image_convert(wrapped, fmt, out);
        ria_image_free(wrapped);
        if (rc == RIA_OK) (*out)->pending_flip = preview->flip;
        return rc;
    }

#ifdef RIA_WITH_JPEG
    return ria_jpeg_decode(preview->data, preview->data_size, fmt,
                           preview->flip, out);
#else
    /* Deliberately not a soft failure that returns something wrong: a caller
     * in a UI toolkit already has a JPEG decoder and should use it on
     * preview->data directly. ria_has_jpeg_support() answers this in advance. */
    return RIA_ERR_UNSUPPORTED;
#endif
}

/* ── Decoding ────────────────────────────────────────────────────────────── */

void ria_decode_options_defaults(ria_decode_options* opt) {
    if (!opt) return;
    memset(opt, 0, sizeof(*opt));

    /*
     * PPG rather than LibRaw's default AHD. Measured on a 33 MP CR3:
     * 1390 ms against 2081 ms, a 1.5x saving on the step that is ~82% of the
     * decode. The cost is small — median per-channel difference from AHD is
     * 1/255, 90th percentile 6 — though the tail lands on high-frequency
     * edges. Callers producing final output should ask for AHD or DCB.
     */
    opt->demosaic = RIA_DEMOSAIC_PPG;
    opt->output_color = RIA_COLORSPACE_SRGB;
    opt->output_bits = 8;
    opt->half_size = 0;
    opt->use_camera_wb = 1;
    opt->use_auto_wb = 0;
    opt->no_auto_bright = 0;
    opt->bright = 1.0f;
    opt->gamma_power = 2.222f;  /* LibRaw's sRGB-like default curve          */
    opt->gamma_slope = 4.5f;
    opt->highlight_mode = 0;
    opt->user_flip = -1;        /* as the camera recorded it                 */
    opt->apply_orientation = 1;
    opt->alpha = 0;
}

void ria_decode_options_scene_linear(ria_decode_options* opt) {
    if (!opt) return;
    ria_decode_options_defaults(opt);

    /* A gamma of 1 with a slope of 1 is the identity curve: LibRaw applies no
     * transfer function and the output is proportional to sensor signal. */
    opt->gamma_power = 1.0f;
    opt->gamma_slope = 1.0f;

    /* 8-bit linear would be a false economy. Linear encoding spends most of
     * its code values in the top stop, so 8 bits leaves the deep shadows with
     * a handful of levels and any lift posterises. 16-bit linear carries 128
     * levels in the -8..-7 EV stop, against 20-30 per stop anywhere in 8-bit
     * gamma. */
    opt->output_bits = 16;

    /* The important one. LibRaw's auto-brightness applies a scene-dependent
     * gain — measured at +1.7 EV on the test frames — so with it enabled,
     * "+1 EV" would mean something different on every file. */
    opt->no_auto_bright = 1;

    /*
     * Clip, not blend. The preset's job is a scene-referred buffer whose 1.0
     * is sensor saturation with no further arithmetic, and highlight_mode 0
     * is the mode that delivers exactly that.
     *
     * Modes above 0 reconstruct the clipped highlights and rescale the whole
     * frame to fit them — 0.5401 on the Nikon test frame and 0.5206 on the
     * Canon, which is where the 0.88 EV and 0.94 EV median shifts recorded in
     * approach.md come from. That rescale is no longer a trap: the decode
     * reports it on ria_image.saturation_level, so a caller who sets
     * highlight_mode 2 gets the recovered detail and keeps a stable EV scale
     * by dividing samples by that value before taking log2. The pixels are
     * left alone deliberately — the recovered highlights reach 1.92 anchor
     * units on the Canon under mode 3, which a 16-bit unsigned buffer cannot
     * hold, so renormalising the buffer would re-clip precisely what the
     * reconstruction recovered.
     *
     * The cost of clipping is real but small: 0.126% of the Canon frame,
     * none of the Nikon.
     */
    opt->highlight_mode = 0;
}

static ria_pixel_format format_for(int colors, int bits, int alpha) {
    if (colors == 1) return bits == 16 ? RIA_FMT_GRAY16 : RIA_FMT_GRAY8;
    if (alpha || colors == 4) return bits == 16 ? RIA_FMT_RGBA16 : RIA_FMT_RGBA8;
    return bits == 16 ? RIA_FMT_RGB16 : RIA_FMT_RGB8;
}

ria_status ria_raw_decode(ria_raw* raw, const ria_decode_options* opt,
                          ria_image** out) {
    if (!raw || !out) return RIA_ERR_INVALID;
    *out = NULL;

    ria_decode_options defaults;
    if (!opt) {
        ria_decode_options_defaults(&defaults);
        opt = &defaults;
    }
    if (opt->output_bits != 8 && opt->output_bits != 16) return RIA_ERR_INVALID;

    ria_status rc = rewind_if_needed(raw);
    if (rc != RIA_OK) return rc;

    libraw_data_t* lr = raw->lr;
    lr->params.user_qual = (int)opt->demosaic;
    lr->params.output_color = (int)opt->output_color;
    lr->params.output_bps = opt->output_bits;
    lr->params.half_size = opt->half_size ? 1 : 0;
    lr->params.use_camera_wb = opt->use_camera_wb ? 1 : 0;
    lr->params.use_auto_wb = opt->use_auto_wb ? 1 : 0;
    lr->params.no_auto_bright = opt->no_auto_bright ? 1 : 0;
    lr->params.bright = opt->bright;
    lr->params.gamm[0] = opt->gamma_power > 0.0f ? 1.0f / opt->gamma_power : 0.0f;
    lr->params.gamm[1] = opt->gamma_slope;
    lr->params.highlight = opt->highlight_mode;
    /* user_flip 0 means "do not rotate"; -1 means "use the camera's value". */
    lr->params.user_flip = opt->apply_orientation ? opt->user_flip : 0;

    const int camera_flip = lr->sizes.flip;

    int err = libraw_unpack(lr);
    if (err != LIBRAW_SUCCESS) {
        raw->consumed = 1;
        return map_libraw_error(err);
    }

    err = libraw_dcraw_process(lr);
    if (err != LIBRAW_SUCCESS) {
        raw->consumed = 1;
        return map_libraw_error(err);
    }

    int mem_err = 0;
    libraw_processed_image_t* img = libraw_dcraw_make_mem_image(lr, &mem_err);
    raw->consumed = 1;
    if (!img) return map_libraw_error(mem_err);

    if (img->type != LIBRAW_IMAGE_BITMAP ||
        (img->bits != 8 && img->bits != 16) ||
        (img->colors != 1 && img->colors != 3 && img->colors != 4)) {
        /* Reading the buffer under a wrong assumption would produce
         * plausible-looking garbage rather than an error, so refuse. */
        libraw_dcraw_clear_mem(img);
        return RIA_ERR_UNSUPPORTED;
    }

    const ria_pixel_format src_fmt = format_for(img->colors, img->bits, 0);
    const ria_pixel_format dst_fmt =
        format_for(img->colors, img->bits, opt->alpha);

    ria_image* wrapped = NULL;
    rc = ria_image_wrap(img->data, img->width, img->height, src_fmt, &wrapped);
    if (rc != RIA_OK) {
        libraw_dcraw_clear_mem(img);
        return rc;
    }

    /* Sanity: LibRaw states data_size itself, and a mismatch means the
     * layout is not what these dimensions imply. */
    if ((size_t)img->data_size < wrapped->data_size) {
        ria_image_free(wrapped);
        libraw_dcraw_clear_mem(img);
        return RIA_ERR_INTERNAL;
    }

    /* ria_image_convert copies, so this is a single pass over the pixels
     * whether or not the format actually changes. */
    rc = ria_image_convert(wrapped, dst_fmt, out);
    ria_image_free(wrapped);
    libraw_dcraw_clear_mem(img);
    if (rc != RIA_OK) return rc;

    (*out)->pending_flip =
        opt->apply_orientation ? RIA_FLIP_NONE : camera_flip;

    /*
     * Record what was actually produced, so the image knows its own domain
     * and the EV-based operations can refuse display-referred input instead
     * of trusting the caller to remember. A gamma of 1 with a slope of 1 is
     * the identity, which is exactly scene-referred linear.
     */
    const int is_linear = (opt->gamma_power == 1.0f && opt->gamma_slope == 1.0f);
    ria_image_set_encoding(*out,
                           is_linear ? RIA_TRANSFER_LINEAR : RIA_TRANSFER_GAMMA,
                           opt->gamma_power, opt->gamma_slope,
                           (ria_colorspace)opt->output_color);

    /*
     * min(pre_mul) after dcraw_process IS the whole-image scale.
     * scale_colors() normalises the multipliers by their minimum when
     * highlight == 0 and by their maximum above it, so this is exactly 1.0
     * under mode 0 and the applied gain under any mode above it. No branch,
     * no reference decode. Verified against the median ratio of paired
     * decodes on two bodies: 0.5401 / 0.5206 predicted, 0.54002 / 0.520502
     * measured.
     */
    float saturation = 1.0f;
    for (int c = 0; c < 4; c++) {
        const float m = lr->color.pre_mul[c];
        if (m > 0.0f && m < saturation) saturation = m;
    }
    ria_image_set_saturation_level(*out, saturation);
    return RIA_OK;
}

ria_status ria_raw_sensor_image(ria_raw* raw, ria_image** out,
                                uint32_t* out_filters, int* out_black,
                                int* out_white) {
    if (!raw || !out) return RIA_ERR_INVALID;
    *out = NULL;

    ria_status rc = rewind_if_needed(raw);
    if (rc != RIA_OK) return rc;

    int err = libraw_unpack(raw->lr);
    if (err != LIBRAW_SUCCESS) {
        raw->consumed = 1;
        return map_libraw_error(err);
    }

    libraw_data_t* lr = raw->lr;
    const uint16_t* src = lr->rawdata.raw_image;
    if (!src) {
        /* Foveon and some multi-channel sensors expose colour planes instead
         * of a single mosaic; those need a different accessor and are not
         * wrapped here. */
        raw->consumed = 1;
        return RIA_ERR_UNSUPPORTED;
    }

    const int w = lr->sizes.raw_width;
    const int h = lr->sizes.raw_height;
    rc = ria_image_alloc(w, h, RIA_FMT_GRAY16, NULL, 0, out);
    if (rc != RIA_OK) {
        raw->consumed = 1;
        return rc;
    }

    /* raw_pitch is in bytes and is not always raw_width * 2. */
    const size_t pitch = lr->sizes.raw_pitch ? lr->sizes.raw_pitch
                                             : (size_t)w * sizeof(uint16_t);
    for (int y = 0; y < h; y++) {
        memcpy((*out)->data + (size_t)y * (*out)->stride,
               (const uint8_t*)src + (size_t)y * pitch, (*out)->stride);
    }
    (*out)->pending_flip = lr->sizes.flip;

    if (out_filters) *out_filters = lr->idata.filters;
    if (out_black) *out_black = (int)lr->color.black;
    if (out_white) *out_white = (int)lr->color.maximum;

    raw->consumed = 1;
    return RIA_OK;
}

/* ── One-shot convenience wrappers ───────────────────────────────────────── */

ria_status ria_read_metadata(const char* path, ria_metadata* out) {
    ria_raw* raw = NULL;
    ria_status rc = ria_raw_open(path, &raw);
    if (rc != RIA_OK) return rc;
    rc = ria_raw_metadata(raw, out);
    ria_raw_close(raw);
    return rc;
}

ria_status ria_read_focus(const char* path, ria_focus_point* out) {
    ria_raw* raw = NULL;
    ria_status rc = ria_raw_open(path, &raw);
    if (rc != RIA_OK) return rc;
    rc = ria_raw_focus(raw, out);
    ria_raw_close(raw);
    return rc;
}

ria_status ria_extract_preview(const char* path, ria_preview** out) {
    ria_raw* raw = NULL;
    ria_status rc = ria_raw_open(path, &raw);
    if (rc != RIA_OK) return rc;
    rc = ria_raw_preview(raw, out);
    ria_raw_close(raw);
    return rc;
}

ria_status ria_decode_file(const char* path, const ria_decode_options* opt,
                           ria_image** out) {
    ria_raw* raw = NULL;
    ria_status rc = ria_raw_open(path, &raw);
    if (rc != RIA_OK) return rc;
    rc = ria_raw_decode(raw, opt, out);
    ria_raw_close(raw);
    return rc;
}
