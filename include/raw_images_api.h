/*
 * raw_images_api.h — camera RAW decoding and image enhancement.
 *
 * A C99 library over LibRaw. Two halves that can be used independently:
 *
 *   1. Reading  — open a RAW file once (ria_raw_open) and pull metadata, the
 *                 embedded preview, autofocus data or a full demosaic off it.
 *   2. Editing  — operate on ria_image buffers: tonal adjustments, colour,
 *                 geometry, sharpening, statistics.
 *
 * Conventions that hold throughout:
 *
 *   - No struct is ever returned or passed by value. Everything crosses the
 *     boundary as a pointer or an int, which keeps the ABI trivial to bind
 *     from Dart FFI, Python ctypes, Rust, Go and friends.
 *   - Every function that allocates has a matching free function, and the
 *     caller owns what it receives.
 *   - Fallible functions return ria_status; RIA_OK is 0 and every error is
 *     negative, so `if (rc)` is a valid error test.
 *   - Output parameters are written only on success, except where documented.
 *   - Public structs are append-only. Fields are added at the end so that a
 *     mirrored struct in another language keeps working.
 *
 * Thread safety: no global mutable state. Distinct ria_raw handles and
 * distinct ria_image buffers may be used concurrently from different threads.
 * A single handle or buffer must not be, without external locking.
 */

#ifndef RAW_IMAGES_API_H
#define RAW_IMAGES_API_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#if defined(__GNUC__) || defined(__clang__)
#  define RIA_API __attribute__((visibility("default")))
#else
#  define RIA_API
#endif

/* ── Version ─────────────────────────────────────────────────────────────── */

#define RIA_VERSION_MAJOR 0
#define RIA_VERSION_MINOR 1
#define RIA_VERSION_PATCH 0

/** "0.1.0" — the library's own version. */
RIA_API const char* ria_version_string(void);

/** The LibRaw version this library was built against, e.g. "0.22.2". */
RIA_API const char* ria_libraw_version_string(void);

/* ── Status codes ────────────────────────────────────────────────────────── */

typedef enum {
    RIA_OK               =  0,
    RIA_ERR_INVALID      = -1,  /* NULL or out-of-range argument             */
    RIA_ERR_MEMORY       = -2,  /* allocation failed                         */
    RIA_ERR_IO           = -3,  /* file missing or unreadable                */
    RIA_ERR_UNSUPPORTED  = -4,  /* format the library will not handle        */
    RIA_ERR_DECODE       = -5,  /* LibRaw rejected the data                  */
    RIA_ERR_NO_DATA      = -6,  /* the file simply has no such record        */
    RIA_ERR_INTERNAL     = -7   /* a bug here, or a LibRaw error with no map */
} ria_status;

/** Human-readable, static storage, never NULL. */
RIA_API const char* ria_status_string(ria_status status);

/* ── Colour encoding ─────────────────────────────────────────────────────── */

/**
 * Which primaries `ria_image.data` is expressed in. Declared here rather than
 * beside the decode options because an image carries it.
 */
typedef enum {
    RIA_COLORSPACE_RAW      = 0,
    RIA_COLORSPACE_SRGB     = 1,
    RIA_COLORSPACE_ADOBE    = 2,
    RIA_COLORSPACE_WIDE     = 3,
    RIA_COLORSPACE_PROPHOTO = 4,
    RIA_COLORSPACE_XYZ      = 5,
    RIA_COLORSPACE_ACES     = 6
} ria_colorspace;

/**
 * The 3x3 converting linear sRGB to `space`, row-major.
 *
 * This is the table the decode applied, so a caller who decoded at
 * output_color = space can invert it and get back to sRGB exactly.
 * RIA_COLORSPACE_RAW has no such matrix and returns RIA_ERR_INVALID.
 */
RIA_API ria_status ria_colorspace_from_srgb(ria_colorspace space,
                                            float matrix[9]);

/**
 * The transfer function encoding an image's sample values.
 *
 * This is the single most consequential field on an image, because it says
 * which of two domains the pixels are in:
 *
 *   - RIA_TRANSFER_LINEAR is **scene-referred**: samples are proportional to
 *     light, so ratios are photometric and an exposure value means something.
 *     Doubling a sample is one stop, exactly.
 *   - Anything else is **display-referred**: samples are code values for a
 *     display. Doubling one is a brightness change of no fixed size, and
 *     arithmetic in stops is meaningless.
 *
 * Operations specified in EV require linear input and say so by returning
 * RIA_ERR_INVALID otherwise, rather than producing a plausible wrong answer.
 */
typedef enum {
    RIA_TRANSFER_LINEAR = 0,  /* scene-referred                              */
    RIA_TRANSFER_SRGB   = 1,  /* the IEC 61966-2-1 piecewise curve           */
    RIA_TRANSFER_GAMMA  = 2   /* power + linear toe; LibRaw's gamm[0], gamm[1] */
} ria_transfer;

/**
 * Encode a linear value, or decode back to linear. `gamma` and `slope` are
 * used only for RIA_TRANSFER_GAMMA — 2.222 and 4.5 reproduce LibRaw's
 * default output curve, which is Rec.709-shaped rather than sRGB-shaped.
 *
 * These are scalar helpers for callers and tests. Bulk conversion goes
 * through a table; see ria_apply_display_transform.
 */
RIA_API float ria_transfer_encode(float linear, ria_transfer, float gamma,
                                  float slope);
RIA_API float ria_transfer_decode(float encoded, ria_transfer, float gamma,
                                  float slope);

/* ── Pixel buffers ───────────────────────────────────────────────────────── */

typedef enum {
    RIA_FMT_RGB8   = 0,   /* 3 x uint8,  interleaved                        */
    RIA_FMT_RGBA8  = 1,   /* 4 x uint8,  interleaved, alpha last            */
    RIA_FMT_RGB16  = 2,   /* 3 x uint16, native byte order                  */
    RIA_FMT_RGBA16 = 3,   /* 4 x uint16, native byte order                  */
    RIA_FMT_GRAY8  = 4,   /* 1 x uint8                                      */
    RIA_FMT_GRAY16 = 5    /* 1 x uint16                                     */
} ria_pixel_format;

RIA_API int ria_format_channels(ria_pixel_format fmt); /* 1, 3 or 4; 0 if invalid */
RIA_API int ria_format_bits(ria_pixel_format fmt);     /* 8 or 16;   0 if invalid */
RIA_API int ria_format_has_alpha(ria_pixel_format fmt);

/**
 * An owned, tightly-interleaved pixel buffer.
 *
 * `data` is a single allocation of `stride * height` bytes. Rows are
 * contiguous — `stride` always equals `width * channels * bits/8` today, but
 * read it rather than recomputing it, so padded buffers stay possible later.
 *
 * The fields are public so that callers can hand `data` straight to a GPU
 * upload, a texture, or another library without a copy.
 */
typedef struct {
    uint8_t*         data;
    int              width;
    int              height;
    int              channels;    /* derived from format; cached for loops   */
    int              bits;        /* 8 or 16                                 */
    size_t           stride;      /* bytes per row                           */
    size_t           data_size;   /* stride * height                         */
    ria_pixel_format format;
    /**
     * The camera orientation baked into these pixels, as a LibRaw flip code
     * (0 upright, 3 = 180, 5 = 90 CCW, 6 = 90 CW). RIA_FLIP_NONE means the
     * pixels are upright — either the camera was, or ria_apply_orientation
     * has already been run. A non-zero value is a *pending* rotation the
     * consumer still has to apply, which is how embedded previews arrive.
     */
    int              pending_flip;

    /**
     * How to read `data`. See ria_transfer — RIA_TRANSFER_LINEAR marks
     * scene-referred pixels, anything else display-referred.
     *
     * `ria_raw_decode` sets this from the options it was given, so a decoded
     * image knows its own domain. Freshly allocated and wrapped images
     * default to sRGB, which is what a caller supplying pixels from a
     * screenshot or an image file almost always has.
     */
    ria_transfer     transfer;
    float            transfer_gamma;   /* RIA_TRANSFER_GAMMA only */
    float            transfer_slope;
    ria_colorspace   colorspace;

    /**
     * The sample value that is sensor saturation, in the units of `data`.
     *
     * 1.0 for any decode that did not renormalise. Below 1.0 when highlight
     * reconstruction rescaled the frame to fit the recovered highlights:
     * LibRaw normalises the white-balance multipliers by their minimum at
     * highlight_mode 0 and by their maximum above it, and this is the ratio.
     * Divide a sample by this before taking log2 and the EV scale is
     * anchored to saturation whatever the highlight mode was.
     */
    float            saturation_level;
} ria_image;

/** Set the encoding fields. Does not touch pixels — it corrects a label. */
RIA_API ria_status ria_image_set_encoding(ria_image*, ria_transfer, float gamma,
                                          float slope, ria_colorspace);

/** Set the saturation anchor. Does not touch pixels — it corrects a label. */
RIA_API ria_status ria_image_set_saturation_level(ria_image*, float level);

#define RIA_FLIP_NONE   0
#define RIA_FLIP_180    3
#define RIA_FLIP_90_CCW 5
#define RIA_FLIP_90_CW  6

/** Allocate `width x height` in `fmt`. Contents are uninitialised. */
RIA_API ria_status ria_image_new(int width, int height, ria_pixel_format fmt,
                                 ria_image** out);

/** As ria_image_new, but zero-filled (transparent black). */
RIA_API ria_status ria_image_new_zeroed(int width, int height,
                                        ria_pixel_format fmt, ria_image** out);

/**
 * Wrap pixels the caller already owns, without copying. The returned image
 * does not own `data`: ria_image_free leaves it alone, and it must outlive
 * the wrapper. `data` must hold at least width*height*channels*bits/8 bytes.
 */
RIA_API ria_status ria_image_wrap(uint8_t* data, int width, int height,
                                  ria_pixel_format fmt, ria_image** out);

/** Deep copy, including pending_flip. */
RIA_API ria_status ria_image_clone(const ria_image* src, ria_image** out);

/** Frees the pixels (unless wrapped) and the struct. NULL is a no-op. */
RIA_API void ria_image_free(ria_image* img);

/**
 * Convert between pixel formats. Widening to alpha fills opaque; dropping it
 * discards. 8 -> 16 scales by 257 so 255 maps to 65535; 16 -> 8 rounds.
 * Colour -> gray uses Rec.709 luma. A same-format conversion is a clone.
 */
RIA_API ria_status ria_image_convert(const ria_image* src,
                                     ria_pixel_format fmt, ria_image** out);

/**
 * Widen packed RGB8 to RGBA8 in place of a copy — the same operation
 * ria_image_convert performs, exposed separately because it is the hot path
 * when handing pixels to a display layer that requires 4 bytes per pixel.
 * `dst` must have room for pixels*4 bytes. Both pointers may be unaligned.
 */
RIA_API void ria_expand_rgb_to_rgba(const uint8_t* src, uint8_t* dst,
                                    size_t pixels);

/* ── Reading RAW files ───────────────────────────────────────────────────── */

/** An open RAW file. Opaque; create with ria_raw_open. */
typedef struct ria_raw ria_raw;

typedef enum {
    RIA_DEMOSAIC_LINEAR = 0,
    RIA_DEMOSAIC_VNG    = 1,
    RIA_DEMOSAIC_PPG    = 2,   /* the default: ~1.5x faster than AHD        */
    RIA_DEMOSAIC_AHD    = 3,
    RIA_DEMOSAIC_DCB    = 4,
    RIA_DEMOSAIC_DHT    = 11,
    RIA_DEMOSAIC_AAHD   = 12
} ria_demosaic;

/**
 * How a full decode should be performed. Fill with ria_decode_options_defaults
 * or ria_decode_options_scene_linear and then override; do not zero it
 * yourself, since 0 is a meaningful value for several fields and the defaults
 * are not all zero.
 */
typedef struct {
    ria_demosaic   demosaic;      /* PPG                                     */
    ria_colorspace output_color;  /* sRGB                                    */
    int            output_bits;   /* 8 or 16. 16 for further processing      */
    int            half_size;     /* 1 = quarter-area, ~4x faster, no demosaic */
    int            use_camera_wb; /* 1 = as-shot white balance               */
    int            use_auto_wb;   /* 1 = grey-world; overridden by camera wb */
    int            no_auto_bright;/* 1 = do not stretch the histogram        */
    float          bright;        /* exposure scale, 1.0 = neutral           */
    float          gamma_power;   /* output gamma, 2.222 = sRGB-ish          */
    float          gamma_slope;   /* toe slope, 4.5 = sRGB-ish               */
    int            highlight_mode;/* 0 clip, 1 unclip, 2 blend, 3+ rebuild   */
    int            user_flip;     /* -1 = as the camera recorded it          */
    int            apply_orientation; /* 1 = bake rotation into the pixels   */
    int            alpha;         /* 1 = return RGBA rather than RGB         */
} ria_decode_options;

/** Display-referred defaults: 8-bit, sRGB-ish gamma, ready to show. */
RIA_API void ria_decode_options_defaults(ria_decode_options* opt);

/**
 * Scene-referred defaults: linear gamma, 16-bit, no auto-brightness,
 * highlight blending. Use this for anything specified in EV.
 *
 * The display-referred defaults are the wrong input for exposure work, and
 * measurably so. On the test frames they clip 1.4–2.3% of the image before
 * any adjustment is applied, and `no_auto_bright = 0` contributes a
 * scene-dependent +1.7 EV — so "+1 EV" would not mean the same thing on two
 * different files. This preset clips 0.00–0.13% and leaves the median around
 * -3.1 EV, with two to three stops of headroom above the 95th percentile.
 *
 * The output needs a display transform before it can be shown; a linear image
 * looks far too dark on a display that expects an encoded one.
 */
RIA_API void ria_decode_options_scene_linear(ria_decode_options* opt);

/** Camera and exposure data. Strings are NUL-terminated, "" when unknown. */
typedef struct {
    char    make[64];
    char    model[64];
    char    software[64];
    char    lens[128];
    char    artist[64];

    float   iso_speed;
    float   shutter;          /* seconds                                     */
    float   aperture;         /* f-number                                    */
    float   focal_len;        /* mm                                          */
    int64_t timestamp;        /* seconds since the Unix epoch, 0 if unknown  */

    /**
     * Dimensions as the image will be *displayed*: transposed relative to the
     * sensor for quarter-turn orientations, so they match what a decode with
     * apply_orientation produces. Reporting the sensor values instead makes
     * every portrait frame claim to be landscape.
     */
    int     width;
    int     height;
    int     raw_width;        /* unrotated active sensor area                */
    int     raw_height;
    int     flip;             /* RIA_FLIP_*                                  */

    int     colors;           /* sensor colour channels, usually 3 or 4      */
    int     black_level;
    int     white_level;
    float   cam_mul[4];       /* as-shot white balance multipliers           */
} ria_metadata;

#define RIA_PREVIEW_JPEG   1   /* data is a JPEG byte stream, undecoded      */
#define RIA_PREVIEW_BITMAP 2   /* data is uncompressed interleaved RGB8      */

/**
 * The camera's own rendering of the frame, embedded in the file. On current
 * bodies this is a near-full-resolution JPEG — 99.7% of the sensor's linear
 * dimensions on the tested Canon and Nikon models — so it is a genuine
 * preview, not a thumbnail.
 *
 * Extracting it is a file read and a memcpy (single-digit milliseconds)
 * against seconds for a demosaic, which makes it the right thing to put on
 * screen first.
 */
typedef struct {
    uint8_t* data;
    size_t   data_size;
    int      format;      /* RIA_PREVIEW_*                                   */
    int      width;
    int      height;
    /** Previews are stored unrotated. The consumer must apply this. */
    int      flip;
} ria_preview;

RIA_API void ria_preview_free(ria_preview* preview);

/**
 * Decode a JPEG preview into pixels. Only available when the library was
 * built with RIA_WITH_JPEG (libjpeg present); otherwise returns
 * RIA_ERR_UNSUPPORTED and the caller should decode the bytes itself, which
 * is usually preferable in a UI toolkit that already has a JPEG decoder.
 * The result has pending_flip set from the preview, not applied.
 */
RIA_API ria_status ria_preview_to_image(const ria_preview* preview,
                                        ria_pixel_format fmt, ria_image** out);

/** True when the build can decode JPEG previews. */
RIA_API int ria_has_jpeg_support(void);

/* ── Autofocus ───────────────────────────────────────────────────────────── */

typedef enum {
    RIA_AF_VENDOR_NONE  = 0,
    RIA_AF_VENDOR_CANON = 1,
    RIA_AF_VENDOR_NIKON = 2
} ria_af_vendor;

/**
 * Where the camera focused, in the vendor's own coordinate system.
 *
 * The raw values are returned deliberately rather than a resolved pixel
 * position: Canon measures from the image centre with a signed Y whose
 * direction varies by product line, Nikon from the top-left corner unsigned.
 * ria_focus_resolve applies the interpretation, and keeping the two apart
 * means a body-specific correction never requires re-reading the file.
 */
typedef struct {
    ria_af_vendor vendor;
    int valid;              /* 0 = no usable AF record; other fields unset   */
    int x, y;               /* vendor coordinates, see above                 */
    int width, height;      /* AF area size, in AF-image space               */
    int af_image_width;     /* the space x/y/width/height are measured in    */
    int af_image_height;
    int flip;               /* RIA_FLIP_*; AF coords are recorded unrotated  */
    int points_in_focus;    /* how many points reported focus (Canon)        */
} ria_focus_point;

/** An axis-aligned rectangle in decoded-image pixels. */
typedef struct {
    double center_x;
    double center_y;
    double width;
    double height;
} ria_focus_area;

/**
 * Whether Canon EOS bodies measure AF Y positions upward from the centre.
 * 1 (the default) is confirmed against EOS R7 frames. PowerShot bodies are
 * documented to use the opposite convention. A wrong setting mirrors the
 * point across the horizontal axis, which looks plausible rather than broken,
 * so change it only on visual evidence. Nikon is unaffected.
 */
RIA_API void ria_focus_set_canon_y_up(int y_is_up);
RIA_API int  ria_focus_get_canon_y_up(void);

/**
 * Resolve vendor coordinates into pixels of a decoded image that is
 * `image_width x image_height` *after* orientation has been applied.
 * Returns RIA_ERR_NO_DATA when the point is not usable.
 */
RIA_API ria_status ria_focus_resolve(const ria_focus_point* focus,
                                     int image_width, int image_height,
                                     ria_focus_area* out);

/* ── Open / read ─────────────────────────────────────────────────────────── */

/**
 * Open a RAW file and parse its headers and MakerNotes. Costs 1-5 ms: no
 * sensor data is read until ria_raw_decode or ria_raw_preview.
 *
 * Hold the handle for as long as you need the file. Metadata, focus and
 * preview reads off one handle avoid re-opening, which is the difference
 * between one header parse and three.
 */
RIA_API ria_status ria_raw_open(const char* path, ria_raw** out);

/** Open from a memory buffer. The buffer must outlive the handle. */
RIA_API ria_status ria_raw_open_buffer(const void* data, size_t size,
                                       ria_raw** out);

RIA_API void ria_raw_close(ria_raw* raw);

/** The path passed to ria_raw_open, or "" for a buffer. Owned by the handle. */
RIA_API const char* ria_raw_path(const ria_raw* raw);

/** Fills a caller-allocated struct. Cheap — the data is already parsed. */
RIA_API ria_status ria_raw_metadata(ria_raw* raw, ria_metadata* out);

/** Cheap: MakerNotes were parsed during open. RIA_ERR_NO_DATA if absent. */
RIA_API ria_status ria_raw_focus(ria_raw* raw, ria_focus_point* out);

/** Extract the embedded preview. RIA_ERR_NO_DATA when the file has none. */
RIA_API ria_status ria_raw_preview(ria_raw* raw, ria_preview** out);

/**
 * Full decode: unpack, demosaic, colour-convert. Seconds for a 24-33 MP
 * frame, dominated by the demosaic, which is memory-bandwidth-bound — 8
 * threads buy only ~2x over 1, so the algorithm choice in `opt` is the lever
 * that matters. Pass NULL for `opt` to use the defaults.
 *
 * May be called repeatedly on one handle with different options; the file is
 * re-read internally, since the demosaic consumes the unpacked data.
 */
RIA_API ria_status ria_raw_decode(ria_raw* raw, const ria_decode_options* opt,
                                  ria_image** out);

/**
 * The unprocessed sensor mosaic: one uint16 per photosite, no demosaic, no
 * white balance, no gamma. This is the input a custom pipeline wants.
 * `out_filters` receives LibRaw's Bayer pattern code (0 for Fuji X-Trans and
 * full-colour sensors); `out_black`/`out_white` the levels to normalise by.
 * Any of the three may be NULL.
 */
RIA_API ria_status ria_raw_sensor_image(ria_raw* raw, ria_image** out,
                                        uint32_t* out_filters,
                                        int* out_black, int* out_white);

/**
 * The raw materials for a colour temperature calculation.
 *
 * LibRaw will not give you a Kelvin value, and neither does this call: it
 * hands back the four inputs from which one is derived, because the derivation
 * is a matter of judgement — which locus, which method, and whether the result
 * is meaningful at all for the illuminant in question.
 *
 * `cam_xyz` is the camera's own characterisation, `cam[i] = sum_j cam_xyz[i][j]
 * * XYZ[j]` for XYZ under D65. Inverting its 3x3 leading block takes a camera
 * neutral to XYZ, and `(1/cam_mul[0], 1/cam_mul[1], 1/cam_mul[2])` is the
 * camera neutral the shot was balanced to — those two together give the
 * as-shot white point.
 *
 * `wbct_rows` is 0 on most bodies. When it is not, `wbct[i][0]` is a colour
 * temperature in Kelvin and `wbct[i][1..4]` the camera's own multipliers for
 * it — the vendor's answer, which beats any colorimetric reconstruction of
 * it. Canon populates this (15 rows, 2400-10900 K on the EOS R7); Nikon
 * does not.
 *
 * Available immediately after open; no unpack or decode is required.
 */
typedef struct {
    float cam_mul[4];      /* as-shot multipliers, camera channel order      */
    float pre_mul[4];      /* LibRaw's daylight multipliers                  */
    float cam_xyz[4][3];   /* XYZ(D65) -> camera; row 3 used on 4-colour CFAs */
    int   colors;          /* sensor colour channels, 3 or 4                 */
    int   wbct_rows;       /* rows populated in `wbct`; 0 when absent        */
    float wbct[64][5];     /* Kelvin, then four multipliers                  */
} ria_color_data;

RIA_API ria_status ria_raw_color_data(ria_raw* raw, ria_color_data* out);

/**
 * The underlying `libraw_data_t*`, for callers that need a LibRaw feature
 * this API does not wrap. Unstable by definition: it exposes LibRaw's ABI,
 * and mutating it can invalidate the handle's own state.
 */
RIA_API void* ria_raw_native_handle(ria_raw* raw);

/** Convenience: open, read, close. For one-shot callers. */
RIA_API ria_status ria_read_metadata(const char* path, ria_metadata* out);
RIA_API ria_status ria_read_focus(const char* path, ria_focus_point* out);
RIA_API ria_status ria_extract_preview(const char* path, ria_preview** out);
RIA_API ria_status ria_decode_file(const char* path,
                                   const ria_decode_options* opt,
                                   ria_image** out);

/** True when the extension is one LibRaw is likely to handle. Case-insensitive. */
RIA_API int ria_is_raw_extension(const char* path);

/** NULL-terminated list of the extensions ria_is_raw_extension accepts. */
RIA_API const char* const* ria_supported_extensions(void);

/* ── The display transform ───────────────────────────────────────────────── */

/**
 * Scene-referred linear in, display-referred encoded out. This is the stage
 * LibRaw normally performs invisibly at the end of a decode; doing it
 * separately is what allows everything in between to work in stops.
 */

typedef enum {
    /**
     * Scale, then hard-clip at white. Reproduces LibRaw's own behaviour, and
     * with the default parameters reproduces it exactly.
     */
    RIA_DISPLAY_CLIP = 0,
    /**
     * Extended Reinhard: `y(1 + y/W²)/(1 + y)`, where W is `white_point`
     * after scaling. Monotonic, leaves the shadows alone, and rolls the
     * highlights off smoothly instead of flattening them to white.
     *
     * Note that with the default white_point of 1.0 this is *identical* to
     * clipping — the curve is the identity when W = 1. The shoulder does work
     * only when there is something above display white to compress, which
     * means either raising white_point or brightening via grey_point.
     */
    RIA_DISPLAY_SHOULDER = 1
} ria_display_mode;

typedef struct {
    ria_display_mode mode;

    /**
     * The scene-linear value placed at display middle grey. Default 0.18,
     * the photographic grey card, which makes the mapping the identity.
     *
     * **This is the brightness control.** Halving it brightens by one stop.
     * It is not called exposure because it is not one: exposure is a scale on
     * scene light, and this is a choice about where to anchor the display.
     * They coincide numerically, and differ in which domain owns them.
     */
    float grey_point;

    /**
     * The scene-linear value placed at display white. Default 1.0 (sensor
     * saturation). Raise it to fit more highlight range into the display —
     * 4.0 compresses two extra stops — which only does anything in
     * RIA_DISPLAY_SHOULDER mode.
     */
    float white_point;

    /** Output encoding. Defaults reproduce LibRaw: gamma 2.222, slope 4.5. */
    ria_transfer transfer;
    float        gamma;
    float        slope;
} ria_display_transform;

RIA_API void ria_display_transform_defaults(ria_display_transform*);

/**
 * Apply the transform, producing a new image in `out_fmt`.
 *
 * The source must be RIA_TRANSFER_LINEAR; anything else returns
 * RIA_ERR_INVALID rather than silently producing a wrong result — a display
 * transform applied twice looks washed out in a way that is easy to mistake
 * for a bad photograph.
 *
 * The whole transform is a one-dimensional function of sample value, so it
 * runs from a table: one lookup per sample, no `powf` in the loop.
 *
 * The shoulder is applied per channel rather than to luminance. That
 * desaturates bright highlights toward white, which is what film does and
 * what viewers expect; the alternative preserves saturation and pushes
 * channels out of gamut. This is the opposite of the choice made for
 * creative tonal controls, where an unrequested hue shift is a defect — the
 * difference is that this stage's job is to fit the scene into the display.
 */
RIA_API ria_status ria_apply_display_transform(const ria_image* scene,
                                               const ria_display_transform*,
                                               ria_pixel_format out_fmt,
                                               ria_image** out);

/* ── Tonal and colour adjustment ─────────────────────────────────────────── */

/**
 * A complete tonal edit, in the **display-referred** domain. Every field is
 * neutral at 0 except the multipliers and gamma, which are neutral at 1 —
 * ria_adjustments_defaults sets that up.
 *
 * These controls are deliberately not specified in stops. They act on encoded
 * code values, where a stop has no fixed size, and they are anchored to the
 * perceptual midpoint of the display range rather than to scene luminance.
 * That makes them the right tool for finishing an image and the wrong tool
 * for exposure. For anything in EV, decode with
 * ria_decode_options_scene_linear and work before the display transform.
 *
 * (There was an `exposure_ev` field here. It multiplied encoded values, so
 * +1 EV on mid-grey clipped to white where the correct answer is 184/255. It
 * was removed rather than repaired: the operation cannot be made correct in
 * this domain, because the data it needs has already been through a tone
 * curve.)
 *
 * The whole struct is applied in a single pass over the pixels. The
 * per-channel parts collapse into one lookup table per channel, built once
 * (256 or 65536 entries), so their cost is independent of how many are used;
 * only saturation and vibrance add per-pixel arithmetic, because they need
 * all three channels at once.
 *
 * Order of operations, on values normalised to [0,1]:
 *   white balance -> black/white point -> shadows/highlights
 *   -> contrast -> gamma -> [saturation, vibrance]
 */
typedef struct {
    float wb_r, wb_g, wb_b;   /* channel multipliers, 1 = neutral            */
    float black_point;        /* [0,1) mapped to black                       */
    float white_point;        /* (0,1] mapped to white                       */
    float shadows;            /* [-1,1] lift or crush the lower midtones     */
    float highlights;         /* [-1,1] recover or push the upper midtones   */
    float contrast;           /* [-1,1] around a 0.5 pivot                   */
    float gamma;              /* >0, applied as v^(1/gamma); 1 = neutral     */
    float saturation;         /* [-1,inf) distance from luma; -1 = greyscale */
    float vibrance;           /* like saturation, weighted toward flat colour*/
} ria_adjustments;

RIA_API void ria_adjustments_defaults(ria_adjustments* adj);

/** True when applying `adj` would leave every pixel unchanged. */
RIA_API int ria_adjustments_is_identity(const ria_adjustments* adj);

/** Apply in place. Alpha is preserved untouched; gray images skip colour ops. */
RIA_API ria_status ria_apply_adjustments(ria_image* img,
                                         const ria_adjustments* adj);

/**
 * Apply a caller-supplied tone curve, one entry per input level (256 for
 * 8-bit, 65536 for 16-bit). `curve` may be NULL for a channel to leave it
 * unchanged. Alpha is never mapped.
 */
RIA_API ria_status ria_apply_curve(ria_image* img, const uint16_t* curve_r,
                                   const uint16_t* curve_g,
                                   const uint16_t* curve_b);

/* ── Statistics ──────────────────────────────────────────────────────────── */

#define RIA_HISTOGRAM_BINS 256

typedef struct {
    uint32_t r[RIA_HISTOGRAM_BINS];
    uint32_t g[RIA_HISTOGRAM_BINS];
    uint32_t b[RIA_HISTOGRAM_BINS];
    uint32_t luma[RIA_HISTOGRAM_BINS];
    uint64_t pixels;
    /** Fraction of pixels at bin 0 / bin 255 in any colour channel. */
    double   clipped_black;
    double   clipped_white;
} ria_histogram;

/** 256 bins regardless of bit depth; 16-bit input is scaled down. */
RIA_API ria_status ria_compute_histogram(const ria_image* img,
                                         ria_histogram* out);

/**
 * Stretch each channel so that `clip_percent` of pixels fall outside the
 * range at each end — the classic auto-levels. 0.5 is a reasonable default;
 * 0 stretches to the true extremes and is sensitive to a single hot pixel.
 * Operates per channel, so it also removes a colour cast.
 */
RIA_API ria_status ria_auto_levels(ria_image* img, float clip_percent);

/**
 * Fill `adj` with an automatic starting point for `img`: exposure and
 * black/white points from the histogram, contrast left alone. Does not
 * modify the image, so a UI can present the numbers before committing.
 */
RIA_API ria_status ria_suggest_adjustments(const ria_image* img,
                                           ria_adjustments* adj);

/* ── Geometry ────────────────────────────────────────────────────────────── */

/**
 * Rotate by a LibRaw flip code into a new image, and clear pending_flip on
 * the result. Codes other than 0/3/5/6 copy unchanged rather than guess.
 */
RIA_API ria_status ria_apply_orientation(const ria_image* src, int flip,
                                         ria_image** out);

/** Copy a rectangle. Must lie fully inside the source. */
RIA_API ria_status ria_crop(const ria_image* src, int x, int y,
                            int width, int height, ria_image** out);

typedef enum {
    /**
     * Triangle filter with a support that widens as the image shrinks: plain
     * bilinear when enlarging, a correct area average when reducing. This is
     * what a thumbnail wants — a bilinear downscale of a 33 MP frame samples
     * a fraction of the pixels and aliases badly.
     */
    RIA_RESIZE_TRIANGLE = 0,
    /** Nearest neighbour. Fast, blocky; for previews of previews. */
    RIA_RESIZE_NEAREST  = 1
} ria_resize_filter;

RIA_API ria_status ria_resize(const ria_image* src, int width, int height,
                              ria_resize_filter filter, ria_image** out);

/** Resize to fit inside a box, preserving aspect ratio. Never enlarges. */
RIA_API ria_status ria_fit_within(const ria_image* src, int max_width,
                                  int max_height, ria_resize_filter filter,
                                  ria_image** out);

/* ── Spatial filters ─────────────────────────────────────────────────────── */

/**
 * Separable Gaussian, in place. Cost is O(pixels * sigma), not O(sigma^2).
 * Alpha is blurred with the colour channels. sigma <= 0 is a no-op.
 */
RIA_API ria_status ria_gaussian_blur(ria_image* img, float sigma);

/**
 * Unsharp mask, in place: img += amount * (img - blur(img, sigma)), applied
 * only where the local difference exceeds `threshold` (in normalised units,
 * 0-1), which is what keeps sharpening out of flat sky and skin.
 *
 * Sensible starting point for a 24 MP frame: sigma 1.0, amount 0.6,
 * threshold 0.01.
 */
RIA_API ria_status ria_unsharp_mask(ria_image* img, float sigma, float amount,
                                    float threshold);

/* ── Writing ─────────────────────────────────────────────────────────────── */

/**
 * Write a binary PPM (P6) or PGM (P5), chosen by channel count. Alpha is
 * dropped. Deliberately the only writer here: it is dependency-free and
 * exists so that tests and CLI tools can dump a result. Real output formats
 * belong to the caller.
 */
RIA_API ria_status ria_write_pnm(const ria_image* img, const char* path);

#ifdef __cplusplus
}
#endif

#endif /* RAW_IMAGES_API_H */
