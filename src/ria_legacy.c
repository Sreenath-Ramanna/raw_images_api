/*
 * ria_legacy.c — implementation of the pre-extraction ABI.
 *
 * See raw_images_api_legacy.h. Every function is a call into the ria_* API
 * plus a struct copy; there is no second implementation of anything.
 */

#include "raw_images_api.h"
#include "raw_images_api_legacy.h"
#include "ria_internal.h"

#include <stdlib.h>
#include <string.h>

RawImageResult* raw_decode_file(const char* path) {
    ria_decode_options opt;
    ria_decode_options_defaults(&opt);
    opt.alpha = 1;          /* the old wrapper always widened RGB to RGBA */
    opt.output_bits = 8;

    ria_image* img = NULL;
    if (ria_decode_file(path, &opt, &img) != RIA_OK) return NULL;

    /* The old contract is RGBA8 or nothing. A monochrome sensor decodes to
     * one channel, which would have failed here before too. */
    if (img->format != RIA_FMT_RGBA8) {
        ria_image_free(img);
        return NULL;
    }

    RawImageResult* result = (RawImageResult*)malloc(sizeof(RawImageResult));
    if (!result) {
        ria_image_free(img);
        return NULL;
    }

    result->width = img->width;
    result->height = img->height;
    result->colors = 4;
    result->bits = 8;
    result->data_size = (int)img->data_size;
    /* Ownership moves to the caller, who frees it with raw_free_result. */
    result->data = ria_image_release_data(img);
    ria_image_free(img);

    return result;
}

void raw_free_result(RawImageResult* result) {
    if (!result) return;
    free(result->data);
    free(result);
}

int raw_read_meta(const char* path, RawImageMeta* out) {
    if (!out) return -1;

    ria_metadata meta;
    if (ria_read_metadata(path, &meta) != RIA_OK) return -1;

    memset(out, 0, sizeof(*out));
    memcpy(out->make, meta.make, sizeof(out->make));
    memcpy(out->model, meta.model, sizeof(out->model));
    out->make[sizeof(out->make) - 1] = '\0';
    out->model[sizeof(out->model) - 1] = '\0';
    out->iso_speed = meta.iso_speed;
    out->shutter = meta.shutter;
    out->aperture = meta.aperture;
    out->focal_len = meta.focal_len;
    out->width = meta.width;
    out->height = meta.height;
    out->flip = meta.flip;
    return 0;
}

RawThumbResult* raw_decode_thumb(const char* path) {
    ria_preview* preview = NULL;
    if (ria_extract_preview(path, &preview) != RIA_OK) return NULL;

    RawThumbResult* out = (RawThumbResult*)malloc(sizeof(RawThumbResult));
    if (!out) {
        ria_preview_free(preview);
        return NULL;
    }

    out->data = preview->data;      /* moved, not copied */
    out->data_size = (int)preview->data_size;
    out->format = preview->format;  /* RIA_PREVIEW_* and RAW_THUMB_* agree */
    out->width = preview->width;
    out->height = preview->height;
    out->flip = preview->flip;

    preview->data = NULL;
    ria_preview_free(preview);
    return out;
}

void raw_free_thumb(RawThumbResult* thumb) {
    if (!thumb) return;
    free(thumb->data);
    free(thumb);
}

int raw_read_focus(const char* path, RawFocusPoint* out) {
    if (!out) return -1;
    memset(out, 0, sizeof(*out));

    ria_raw* raw = NULL;
    if (ria_raw_open(path, &raw) != RIA_OK) return -1;

    ria_focus_point focus;
    const ria_status rc = ria_raw_focus(raw, &focus);
    ria_raw_close(raw);

    /* The old contract: opening the file is what "success" meant. A frame
     * with no AF record comes back valid = 0, not as an error. */
    if (rc != RIA_OK && rc != RIA_ERR_NO_DATA) return -1;

    out->vendor = (int)focus.vendor;
    out->valid = focus.valid;
    out->x = focus.x;
    out->y = focus.y;
    out->width = focus.width;
    out->height = focus.height;
    out->af_image_width = focus.af_image_width;
    out->af_image_height = focus.af_image_height;
    out->flip = focus.flip;
    out->points_in_focus = focus.points_in_focus;
    return 0;
}
