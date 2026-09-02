/*
 * raw_images_api_legacy.h — the ABI raw_viewer's libraw_wrapper.so exported
 * before this library existed.
 *
 * Kept so that an existing FFI binding keeps working across the extraction
 * without editing its struct mirrors. Every function here is a thin call
 * into the ria_* API; nothing is implemented twice.
 *
 * Do not extend this file. New work goes in raw_images_api.h — this surface
 * has no error reporting, no 16-bit output, no processing, and its structs
 * cannot change without breaking the callers it exists to serve.
 *
 * Built only when RIA_BUILD_LEGACY_ABI is on (the default).
 */

#ifndef RAW_IMAGES_API_LEGACY_H
#define RAW_IMAGES_API_LEGACY_H

#ifdef __cplusplus
extern "C" {
#endif

/* 32 bytes on LP64. Field order is load-bearing: FFI consumers mirror it. */
typedef struct {
    unsigned char* data;      /* RGBA8, tightly packed                       */
    int            width;
    int            height;
    int            colors;    /* always 4                                    */
    int            bits;      /* always 8                                    */
    int            data_size; /* width * height * 4                          */
} RawImageResult;

#define RAW_THUMB_JPEG   1
#define RAW_THUMB_BITMAP 2

/* 32 bytes on LP64. */
typedef struct {
    unsigned char* data;
    int            data_size;
    int            format;    /* RAW_THUMB_*                                 */
    int            width;
    int            height;
    int            flip;      /* orientation, NOT applied                    */
} RawThumbResult;

/* 156 bytes. */
typedef struct {
    char  make[64];
    char  model[64];
    float iso_speed;
    float shutter;
    float aperture;
    float focal_len;
    int   width;              /* as displayed, orientation applied           */
    int   height;
    int   flip;
} RawImageMeta;

#define RAW_AF_VENDOR_NONE  0
#define RAW_AF_VENDOR_CANON 1
#define RAW_AF_VENDOR_NIKON 2

/* 40 bytes. */
typedef struct {
    int vendor;
    int valid;
    int x, y;
    int width, height;
    int af_image_width;
    int af_image_height;
    int flip;
    int points_in_focus;
} RawFocusPoint;

/** Full decode to 8-bit RGBA. NULL on any failure. Free with raw_free_result. */
RawImageResult* raw_decode_file(const char* path);
void            raw_free_result(RawImageResult* result);

/** 0 on success, -1 on failure. Fills a caller-allocated struct. */
int raw_read_meta(const char* path, RawImageMeta* out);

/** The embedded preview, undecoded. NULL when absent. Free with raw_free_thumb. */
RawThumbResult* raw_decode_thumb(const char* path);
void            raw_free_thumb(RawThumbResult* thumb);

/**
 * 0 if the file could be opened, -1 otherwise. A file with no AF record is
 * not an error: it comes back with `valid` 0.
 */
int raw_read_focus(const char* path, RawFocusPoint* out);

#ifdef __cplusplus
}
#endif

#endif /* RAW_IMAGES_API_LEGACY_H */
