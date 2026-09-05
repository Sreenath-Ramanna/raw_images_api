# raw_images_api — API reference

A C library for reading camera RAW files and enhancing the images that come
out of them. Built on [LibRaw](https://www.libraw.org/); everything public is
plain C99 with a flat, FFI-friendly ABI.

The single public header is `raw_images_api.h`.

```c
#include <raw_images_api.h>

ria_raw* raw;
if (ria_raw_open("DSC_0001.NEF", &raw) != RIA_OK) return;

ria_metadata meta;
ria_raw_metadata(raw, &meta);            /* microseconds — already parsed  */

ria_image* img;
ria_raw_decode(raw, NULL, &img);         /* seconds — the demosaic         */
ria_raw_close(raw);

ria_adjustments adj;
ria_adjustments_defaults(&adj);
adj.contrast = 0.15f;
adj.vibrance = 0.25f;
ria_apply_adjustments(img, &adj);

ria_unsharp_mask(img, 1.0f, 0.6f, 0.01f);
ria_write_pnm(img, "out.ppm");
ria_image_free(img);
```

---

## Contents

1. [The model](#1-the-model)
2. [Conventions](#2-conventions)
3. [Building and linking](#3-building-and-linking)
4. [Status and version](#4-status-and-version)
5. [Images](#5-images) — `ria_image`, formats, conversion
6. [Reading RAW files](#6-reading-raw-files) — `ria_raw`, metadata, previews, decoding
7. [Autofocus](#7-autofocus)
8. [Scene-referred work](#8-scene-referred-work) — linear decoding, the saturation anchor, working-space matrices, the display transform
9. [Adjustment and colour](#9-adjustment-and-colour)
10. [Statistics](#10-statistics)
11. [Geometry](#11-geometry)
12. [Spatial filters](#12-spatial-filters)
13. [Output](#13-output)
14. [The legacy ABI](#14-the-legacy-abi)
15. [Performance](#15-performance)
16. [Binding from another language](#16-binding-from-another-language)
17. [Extending the library](#17-extending-the-library)

---

## 1. The model

Two halves that can be used independently.

**Reading** is built around `ria_raw`, an open file. Opening parses the
headers and MakerNotes — a millisecond or two — and everything cheap
(metadata, the focus point, the embedded preview) is then available without
touching the sensor data. Only `ria_raw_decode` pays for the demosaic.

**Editing** is built around `ria_image`, an owned pixel buffer. Operations
either transform one in place (adjustments, filters) or produce a new one
(conversion, geometry). Nothing in this half knows about LibRaw, so a buffer
from anywhere — a JPEG decoder, a screenshot, a test pattern — can be fed
through it via `ria_image_wrap`.

```
  file ──► ria_raw ──┬──► ria_metadata      1-5 ms
                     ├──► ria_focus_point   1-5 ms
                     ├──► ria_preview       3-7 ms   (camera's own JPEG)
                     └──► ria_image         1.5-3 s  (demosaic)
                                │
   ria_image ◄──────────────────┘
       │
       ├── adjustments, curves          in place, one pass
       ├── auto levels, histogram
       ├── crop, orientation, resize    new image
       ├── blur, unsharp mask           in place
       └── ria_write_pnm
```

## 2. Conventions

These hold everywhere, so they are stated once rather than repeated per
function.

**Errors.** Every fallible function returns `ria_status`. `RIA_OK` is 0 and
every error is negative, so `if (rc)` is a valid test. `ria_status_string`
turns one into text. Output parameters are written only on success — a failed
call leaves your pointer as it was, and functions that allocate set it to
`NULL` first so a stale value cannot survive.

**Ownership.** Every function that allocates has a matching free function, and
what you receive is yours: `ria_image_free`, `ria_preview_free`,
`ria_raw_close`. All three accept `NULL`. Nothing is reference counted; there
are no hidden caches and no global pools.

**No struct passing.** Nothing is returned or taken by value. Struct-return
ABI is the fiddliest thing to get right across a foreign function interface,
so it never happens here.

**Struct layout is append-only.** New fields go at the end. Inserting one in
the middle silently corrupts every field after it in a mirrored struct in
another language, with no error anywhere.

**Threads.** There is no global mutable state except
`ria_focus_set_canon_y_up`, which is a per-camera-family correction rather
than per-image state. Distinct handles and distinct images can be used
concurrently from different threads; a single one cannot, without your own
lock. Decoding two files at once on two threads is the intended pattern.

**In-place versus new.** An operation that cannot change the geometry
(`ria_apply_adjustments`, `ria_gaussian_blur`, `ria_auto_levels`) modifies the
image you give it. One that can (`ria_resize`, `ria_crop`,
`ria_apply_orientation`, `ria_image_convert`) allocates a new one and leaves
the source untouched. The signature says which: an `ria_image**` out-parameter
means a new allocation you now own.

## 3. Building and linking

Requires CMake 3.13+, a C11 compiler, and LibRaw's development headers.

```bash
sudo dnf install LibRaw-devel cmake ninja-build          # Fedora
sudo apt-get install libraw-dev cmake ninja-build        # Debian / Ubuntu

cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
./build/ria_tests                    # 304 checks, no camera files needed
sudo cmake --install build
```

Link with pkg-config (`pkg-config --cflags --libs raw_images_api`) or CMake:

```cmake
find_package(raw_images_api REQUIRED)
target_link_libraries(myapp PRIVATE raw_images_api::raw_images_api)
```

Or embed the source tree directly, which is what `raw_viewer` does:

```cmake
set(RIA_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(RIA_BUILD_TESTS    OFF CACHE BOOL "" FORCE)
set(RIA_INSTALL        OFF CACHE BOOL "" FORCE)
add_subdirectory(path/to/raw_images_api ${CMAKE_BINARY_DIR}/raw_images_api)
```

| CMake option | Default | Effect |
|---|---|---|
| `RIA_BUILD_SHARED` | ON | shared library; OFF builds a static one |
| `RIA_BUILD_LEGACY_ABI` | ON | also export the `raw_*` symbols of [§14](#14-the-legacy-abi) |
| `RIA_BUILD_EXAMPLES` | ON | build `ria_tool` |
| `RIA_BUILD_TESTS` | ON | build `ria_tests` |
| `RIA_USE_OPENMP` | ON | parallelise the pixel loops if OpenMP is present |
| `RIA_INSTALL` | ON | generate install rules |

Without OpenMP everything still works, just serially. `libraw_r` (the
thread-safe LibRaw build) is preferred over `libraw` when both are present.

## 4. Status and version

```c
const char* ria_version_string(void);          /* "0.1.0"                  */
const char* ria_libraw_version_string(void);   /* "0.22.2-Release"         */
const char* ria_status_string(ria_status);     /* never NULL               */
```

| status | meaning |
|---|---|
| `RIA_OK` | 0, success |
| `RIA_ERR_INVALID` | a NULL or out-of-range argument |
| `RIA_ERR_MEMORY` | allocation failed |
| `RIA_ERR_IO` | file missing or unreadable |
| `RIA_ERR_UNSUPPORTED` | a format the library will not handle |
| `RIA_ERR_DECODE` | LibRaw rejected the data |
| `RIA_ERR_NO_DATA` | the file simply has no such record |
| `RIA_ERR_INTERNAL` | a bug here, or a LibRaw error with no mapping |

`RIA_ERR_NO_DATA` is worth distinguishing from the rest: a frame shot on
manual focus has no AF record, and a file may carry no embedded preview.
Neither is a failure of the file or the call.

## 5. Images

```c
typedef struct {
    uint8_t*         data;
    int              width, height;
    int              channels;     /* 1, 3 or 4                            */
    int              bits;         /* 8 or 16                              */
    size_t           stride;       /* bytes per row                        */
    size_t           data_size;    /* stride * height                      */
    ria_pixel_format format;
    int              pending_flip; /* rotation NOT yet applied             */
    ria_transfer     transfer;     /* which domain the samples are in      */
    float            transfer_gamma, transfer_slope;
    ria_colorspace   colorspace;
    float            saturation_level; /* the sample value that is saturation */
} ria_image;
```

The fields are public so that `data` can go straight to a texture upload or
another library without a copy. Rows are contiguous and `stride` is currently
always `width * channels * bits/8` — read the field rather than recomputing
it, so that padded buffers remain possible later.

`transfer` says whether the samples are scene-referred (`RIA_TRANSFER_LINEAR`,
proportional to light, where a stop is a stop) or display-referred (anything
else). It is the field the EV-based operations check before agreeing to run.
See [§8](#8-scene-referred-work).

`saturation_level` says which sample value is sensor saturation. It is `1.0f`
for every image except one a highlight-reconstructing decode rescaled, and
**a sample means an EV only after dividing by it** — see
[§8](#8-scene-referred-work). Like `transfer`, it travels with the buffer
through crop, resize, rotate and format conversion, so a derived image keeps
the EV scale of the frame it came from.

`pending_flip` is a LibRaw orientation code (`RIA_FLIP_NONE`, `RIA_FLIP_180`,
`RIA_FLIP_90_CCW` = 5, `RIA_FLIP_90_CW` = 6). Non-zero means *the consumer
still has to rotate these pixels*. Decodes come back at
`RIA_FLIP_NONE` by default; embedded previews do not, because cameras store
them unrotated. This asymmetry is the most common source of sideways images
in code built on RAW files, which is why the flag travels with the buffer
instead of being remembered separately.

### Formats

| `ria_pixel_format` | channels | bits |
|---|---|---|
| `RIA_FMT_RGB8` | 3 | 8 |
| `RIA_FMT_RGBA8` | 4 | 8 |
| `RIA_FMT_RGB16` | 3 | 16 |
| `RIA_FMT_RGBA16` | 4 | 16 |
| `RIA_FMT_GRAY8` | 1 | 8 |
| `RIA_FMT_GRAY16` | 1 | 16 |

16-bit samples are in native byte order. Alpha is last and is never
premultiplied; nothing in this library generates a non-opaque alpha, but one
you supply is carried through geometry and blur, and left alone by tonal
adjustment and sharpening.

```c
int ria_format_channels(ria_pixel_format);   /* 0 if the value is invalid  */
int ria_format_bits(ria_pixel_format);
int ria_format_has_alpha(ria_pixel_format);
```

### Allocation

```c
ria_status ria_image_new(int width, int height, ria_pixel_format, ria_image**);
ria_status ria_image_new_zeroed(int width, int height, ria_pixel_format, ria_image**);
ria_status ria_image_wrap(uint8_t* data, int w, int h, ria_pixel_format, ria_image**);
ria_status ria_image_clone(const ria_image*, ria_image**);
void       ria_image_free(ria_image*);
```

`ria_image_new` leaves the pixels uninitialised. `ria_image_wrap` borrows a
buffer you own: nothing is copied, `ria_image_free` will not release it, and
it must outlive the wrapper. That is the entry point for pixels from
elsewhere — every operation in this library works on a wrapped buffer exactly
as on an allocated one.

Dimensions are validated, including for overflow: a corrupt header offering
`width * height * channels` larger than `size_t` gets `RIA_ERR_INVALID` rather
than a buffer far smaller than every loop assumes.

### Conversion

```c
ria_status ria_image_convert(const ria_image* src, ria_pixel_format, ria_image** out);
void       ria_expand_rgb_to_rgba(const uint8_t* src, uint8_t* dst, size_t pixels);
```

Adding alpha fills opaque; dropping it discards. 8→16 scales by 257 so that
255 maps exactly onto 65535, which makes 8→16→8 lossless. 16→8 rounds. Colour
to grey uses Rec.709 luma — the same coefficients as the luma histogram and
the saturation control, so a round trip through them does not shift
brightness.

`ria_expand_rgb_to_rgba` is the RGB8→RGBA8 case as a raw pointer call, for
when you have bytes rather than an `ria_image` — a display layer that requires
four bytes per pixel is the usual reason. `ria_image_convert` uses it
internally.

## 6. Reading RAW files

### Opening

```c
ria_status  ria_raw_open(const char* path, ria_raw** out);
ria_status  ria_raw_open_buffer(const void* data, size_t size, ria_raw** out);
void        ria_raw_close(ria_raw*);
const char* ria_raw_path(const ria_raw*);
void*       ria_raw_native_handle(ria_raw*);
```

Opening parses headers and MakerNotes and costs 1–2 ms. No sensor data is
read until you ask for pixels.

Hold the handle for as long as you need the file. Metadata, focus and preview
off one handle is one header parse; the same three through the one-shot
convenience wrappers is three.

`ria_raw_open_buffer` does not copy — the buffer must outlive the handle.

`ria_raw_native_handle` returns the underlying `libraw_data_t*` for anything
this API does not wrap. It is unstable by definition, exposes LibRaw's ABI
directly, and mutating it can invalidate the handle's own bookkeeping.

**Decoding is destructive, and the handle hides it.** LibRaw's sequence
(open → unpack → process) consumes the unpacked data, so a second decode on
the same object would fail. `ria_raw_decode` detects this and re-reads the
file transparently, which is what makes "show me this again at 16 bits" or
"re-decode with AHD" work without the caller tracking state. The cost is
re-reading the file; the alternative was making every caller close and reopen.

### Metadata

```c
ria_status ria_raw_metadata(ria_raw*, ria_metadata* out);
```

Fills a caller-allocated struct: make, model, software, lens, artist, ISO,
shutter, aperture, focal length, timestamp, dimensions, orientation, sensor
colour count, black and white levels, and the as-shot white balance
multipliers. Strings are NUL-terminated and empty when unknown.

`width`/`height` are the dimensions **as displayed** — already transposed for
quarter-turn orientations, so they match what a decode produces.
`raw_width`/`raw_height` are the unrotated sensor area. Reporting the sensor
values as the image size is the classic bug that makes every portrait frame
claim to be landscape.

### Embedded previews

```c
ria_status ria_raw_preview(ria_raw*, ria_preview** out);
void       ria_preview_free(ria_preview*);
ria_status ria_preview_to_image(const ria_preview*, ria_pixel_format, ria_image**);
int        ria_has_jpeg_support(void);
```

Cameras embed their own rendering of the frame. On both tested bodies it is
about 99.7% of full resolution — 6048×4024 inside a 6064×4040 NEF — so it is a
genuine preview, not a thumbnail. Extracting it is a file read and a memcpy:
**3–7 ms**, against 1.5–3 s for a demosaic. If something has to appear on a
screen quickly, this is it.

`ria_preview` carries `data`, `data_size`, `format` (`RIA_PREVIEW_JPEG` or
`RIA_PREVIEW_BITMAP`), `width`, `height` and `flip`.

JPEG bytes are returned **undecoded**, and `ria_preview_to_image` only decodes
them when the library was built against libjpeg (`ria_has_jpeg_support`
reports whether it was; without it, that call returns `RIA_ERR_UNSUPPORTED`
for a JPEG preview and still handles a bitmap one). This is deliberate: a UI
toolkit already has a JPEG decoder, usually a better-optimised one, and
routing the bytes through it avoids a dependency here for no gain.

The preview is stored unrotated, so `flip` is *pending* — apply it with
`ria_apply_orientation`, or by rotating the texture you draw it into.

### Decoding

```c
void       ria_decode_options_defaults(ria_decode_options*);
ria_status ria_raw_decode(ria_raw*, const ria_decode_options*, ria_image** out);
```

Pass `NULL` for the options to take the defaults. Fill the struct with
`ria_decode_options_defaults` before changing anything — zeroing it yourself
produces a very different result, since 0 is meaningful for several fields.

| field | default | notes |
|---|---|---|
| `demosaic` | `RIA_DEMOSAIC_PPG` | see below |
| `output_color` | `RIA_COLORSPACE_SRGB` | also Adobe, ProPhoto, XYZ, ACES |
| `output_bits` | 8 | 16 for further processing |
| `half_size` | 0 | 1 skips the demosaic entirely: quarter area, ~3x faster |
| `use_camera_wb` | 1 | as-shot white balance |
| `use_auto_wb` | 0 | grey-world; camera WB wins if both are set |
| `no_auto_bright` | 0 | 1 disables LibRaw's histogram stretch |
| `bright` | 1.0 | exposure scale |
| `gamma_power`, `gamma_slope` | 2.222, 4.5 | the sRGB-like output curve |
| `highlight_mode` | 0 | 0 clip, 1 unclip, 2 blend, 3+ rebuild; above 0 the applied rescale comes back on `ria_image.saturation_level` |
| `user_flip` | -1 | -1 = as the camera recorded it |
| `apply_orientation` | 1 | 0 returns unrotated pixels with `pending_flip` set |
| `alpha` | 0 | 1 returns RGBA instead of RGB |

**On the demosaic default.** PPG rather than LibRaw's default AHD, measured on
a 33 MP CR3 at 1390 ms against 2081 ms — a 1.5x saving on the step that is
~82% of the decode. The quality cost was measured rather than assumed: median
per-channel difference from AHD is 1/255 and the 90th percentile is 6, but the
tail lands on high-frequency edges. That trade suits a viewer or a culling
tool. For final output, ask for `RIA_DEMOSAIC_AHD` or `RIA_DEMOSAIC_DCB`.

Throwing cores at the demosaic does not help much: 8 threads give 2.07x over
1, so it is memory-bandwidth-bound. The algorithm is the lever that matters.

### The sensor mosaic

```c
ria_status ria_raw_sensor_image(ria_raw*, ria_image** out, uint32_t* filters,
                                int* black, int* white);
```

One `uint16` per photosite, as recorded: no demosaic, no white balance, no
gamma. This is the input a custom pipeline wants. `filters` is LibRaw's Bayer
pattern code (0 for X-Trans and full-colour sensors), `black` and `white` the
levels to normalise against. Any of the three may be `NULL`.

Returns `RIA_ERR_UNSUPPORTED` for sensors that expose colour planes rather
than a single mosaic, such as Foveon.

### One-shot wrappers

```c
ria_status ria_read_metadata(const char* path, ria_metadata*);
ria_status ria_read_focus(const char* path, ria_focus_point*);
ria_status ria_extract_preview(const char* path, ria_preview**);
ria_status ria_decode_file(const char* path, const ria_decode_options*, ria_image**);
```

Open, read, close. Convenient for a single fact about a single file; use a
handle when you want more than one.

### Recognising files

```c
int                ria_is_raw_extension(const char* path);
const char* const* ria_supported_extensions(void);  /* NULL-terminated */
```

An extension test, not a content test — for populating a file dialog or
filtering a directory listing. Case-insensitive, and it only considers the
basename, so a directory called `shoot.raw` does not make everything inside it
look like a RAW file. LibRaw opens considerably more than this list; the list
is what is worth *offering*.

## 7. Autofocus

```c
ria_status ria_raw_focus(ria_raw*, ria_focus_point* out);
ria_status ria_focus_resolve(const ria_focus_point*, int image_width,
                             int image_height, ria_focus_area* out);
void       ria_focus_set_canon_y_up(int);
int        ria_focus_get_canon_y_up(void);
```

Canon and Nikon record where the camera focused in MakerNote blobs. Reading
them is free — they are parsed during `ria_raw_open`, so no unpack is needed.
Other vendors return `RIA_ERR_NO_DATA`, as do manual-focus frames and adapted
lenses; that is not an error.

Parsing and interpretation are kept apart on purpose. `ria_raw_focus` returns
the vendor's own numbers, `ria_focus_resolve` turns them into pixels:

- **Canon** measures from the image centre with a signed Y. Multiple points
  can report focus; they are averaged, which keeps a multi-point result
  centred rather than arbitrarily picking the first.
- **Nikon** measures from the top-left corner, unsigned, one point.
- Both record against the **unrotated** sensor, so `ria_focus_resolve` undoes
  the image's rotation, scales AF space onto image space (they differ — AF
  space matches the embedded preview), and rotates the result back.

`ria_focus_set_canon_y_up` exists because the references disagree on Canon's Y
direction. The default (1, positive Y is up) is confirmed against EOS R7
frames; PowerShot bodies are documented to use the opposite convention. A
wrong setting mirrors the point across the horizontal axis, which looks
plausible rather than broken — change it only on visual evidence.

`ria_focus_area` is a centre, a width and a height, in pixels of the decoded
image.

## 8. Scene-referred work

An exposure value is a ratio of *linear* light. `ria_raw_decode`'s default
output is display-referred — LibRaw has applied a tone curve and clipped the
highlights — so arithmetic in stops on that output is meaningless. Measured on
the test frames, the default decode clips **1.4–2.3 %** of the image before any
adjustment, and `no_auto_bright = 0` contributes a scene-dependent **+1.7 EV**,
so "+1 EV" would not even mean the same thing on two different files.

Every image therefore carries its own domain:

```c
typedef enum {
    RIA_TRANSFER_LINEAR = 0,   /* scene-referred: a stop is a stop */
    RIA_TRANSFER_SRGB   = 1,
    RIA_TRANSFER_GAMMA  = 2,   /* LibRaw's power-plus-toe curve    */
} ria_transfer;
```

`ria_image.transfer` is set by the decode from the options it was given.
Operations specified in EV require `RIA_TRANSFER_LINEAR` and return
`RIA_ERR_INVALID` otherwise, rather than producing a plausible wrong answer.

### The pipeline

```c
ria_decode_options opt;
ria_decode_options_scene_linear(&opt);   /* linear, 16-bit, no auto-bright */

ria_image* scene;
ria_decode_file("photo.CR3", &opt, &scene);
/* ... scene-referred work happens here ... */

ria_display_transform dt;
ria_display_transform_defaults(&dt);
dt.grey_point = 0.18f / 2.0f;            /* +1 EV */

ria_image* shown;
ria_apply_display_transform(scene, &dt, RIA_FMT_RGB8, &shown);
```

`ria_decode_options_scene_linear` sets `gamma 1.0`, `output_bits 16`,
`no_auto_bright 1` and `highlight_mode 0`. The last is deliberate: modes above
0 renormalise the whole frame to fit reconstructed highlights, so 1.0 would no
longer mean sensor saturation without further arithmetic. Measured with mode 0,
the median lands at −3.11 EV and −3.09 EV on two different cameras and the
anchor holds. The cost is 0.126 % clipping on one test frame.

**A decode above `highlight_mode 0` reports its rescale.** The applied scale
comes back on `ria_image.saturation_level` — measured 0.5401 and 0.5206 on the
two test bodies — and dividing a sample by it before taking `log2` gives an EV
anchored to saturation whichever mode produced the pixels:

```c
opt.highlight_mode = 2;                  /* LibRaw's blend reconstruction */
ria_decode_file("photo.CR3", &opt, &scene);

const float ev = log2f(sample / scene->saturation_level);
```

The pixels are deliberately left alone. Under mode 3 the recovered highlights
reach 1.92 anchor units on one test frame, which a 16-bit unsigned buffer
cannot hold — rescaling the buffer back to the anchor would re-clip exactly
what the reconstruction recovered.

### Working-space matrices

```c
ria_status ria_colorspace_from_srgb(ria_colorspace space, float matrix[9]);
```

The 3×3 converting linear sRGB to `space`, row-major. This is LibRaw's own
`out_rgb[]` table — the matrix the decode applied — so a caller who decoded at
`output_color = space` can invert it and get back to sRGB exactly, which is
what makes a wide-gamut decode deliverable to an sRGB display.
`RIA_COLORSPACE_RAW` is camera primaries, which vary per body and are not in
the table: it returns `RIA_ERR_INVALID`, as does any value outside 1…6.

16-bit is not optional here. Linear encoding spends its code values unevenly —
32768 in the top stop, 128 in the −8…−7 EV stop — and 8-bit linear leaves the
shadows with a handful of levels, so any lift posterises. 16-bit linear still
beats 8-bit gamma at every point down to about −10 EV.

### The display transform

```c
ria_status ria_apply_display_transform(const ria_image* scene,
                                       const ria_display_transform*,
                                       ria_pixel_format out_fmt,
                                       ria_image** out);
```

| field | default | meaning |
|---|---|---|
| `mode` | `RIA_DISPLAY_CLIP` | or `RIA_DISPLAY_SHOULDER` for a highlight rolloff |
| `grey_point` | 0.18 | the scene-linear value placed at display middle grey |
| `white_point` | 1.0 | the scene-linear value placed at display white |
| `transfer`, `gamma`, `slope` | gamma 2.222 / 4.5 | the output curve |

With the defaults the tonal mapping is the identity and the stage is just the
output curve, which **reproduces a LibRaw decode**: verified against a direct
gamma decode of the same file at a mean difference of 0.19 code values, worst
case 1.

**`grey_point` is the brightness control.** Halving it brightens by exactly one
stop, because the data is linear. It is not called exposure because it is not
one — exposure is a scale on scene light and this is a choice about where to
anchor the display — but on linear input the two coincide numerically.

`RIA_DISPLAY_SHOULDER` applies extended Reinhard, `y(1 + y/W²)/(1 + y)`, where
W is `white_point` after scaling. Note that **at `white_point = 1.0` this is
exactly the identity**, so the shoulder does nothing until there is something
above display white to compress — raise `white_point`, or brighten via
`grey_point`. Demonstrated end to end: on a test frame, `+1.7 EV` clipped
0.15 % of pixels while `+2.5 EV` with `white_point = 3.0` reached the same mean
brightness and clipped **0.00 %**.

The shoulder is applied per channel, which desaturates bright highlights toward
white — what film does and what viewers expect. That is the opposite of the
choice made for creative tonal controls, where an unrequested hue shift is a
defect; the difference is that this stage's job is to fit the scene into the
display.

### Scalar helpers

```c
float ria_transfer_encode(float linear, ria_transfer, float gamma, float slope);
float ria_transfer_decode(float encoded, ria_transfer, float gamma, float slope);
ria_status ria_image_set_encoding(ria_image*, ria_transfer, float gamma,
                                  float slope, ria_colorspace);
ria_status ria_image_set_saturation_level(ria_image*, float level);
```

Both setters correct a *label* and touch no pixels. `level` must be greater
than zero — it is a divisor — and anything else is refused rather than stored.

`RIA_TRANSFER_GAMMA` is LibRaw's curve, a power function with a **linear toe**
— not a pure power. The toe matters: ignoring it puts `+1 EV` on encoded 128 at
174 rather than the correct 184.

## 9. Adjustment and colour

```c
void       ria_adjustments_defaults(ria_adjustments*);
int        ria_adjustments_is_identity(const ria_adjustments*);
ria_status ria_apply_adjustments(ria_image*, const ria_adjustments*);
ria_status ria_apply_curve(ria_image*, const uint16_t* r, const uint16_t* g,
                           const uint16_t* b);
```

One struct describes a complete tonal edit, applied in a single pass:

| field | neutral | range | effect |
|---|---|---|---|
| `wb_r`, `wb_g`, `wb_b` | 1.0 | > 0 | per-channel multipliers |
| `black_point` | 0 | [0,1) | input level mapped to black |
| `white_point` | 1 | (0,1] | input level mapped to white |
| `shadows` | 0 | -1..1 | positive lifts the lower midtones |
| `highlights` | 0 | -1..1 | negative recovers the upper midtones |
| `contrast` | 0 | -1..1 | around a 0.5 pivot |
| `gamma` | 1 | > 0 | applied as `v^(1/gamma)` |
| `saturation` | 0 | -1.. | distance from luma; -1 is greyscale |
| `vibrance` | 0 | -1.. | saturation weighted toward flat colour |

Applied in that order, on values normalised to [0,1].

**These controls are display-referred, and none of them is in stops.** They
act on encoded code values, where a stop has no fixed size. There was an
`exposure_ev` field here; it multiplied encoded values, so `+1 EV` on mid-grey
gave 255 where the correct answer is 184 — 0.93 EV too bright. It was removed
rather than repaired, because the operation cannot be made correct in this
domain. For anything in stops, see [§8](#8-scene-referred-work) — decode
linear and work before the display transform.

**Why one struct rather than ten functions.** Every control above except
saturation and vibrance acts on one channel at a time, independently of the
others — so the whole chain is a function from input level to output level.
There are only 256 or 65536 possible levels, so it collapses into one lookup
table per channel, built once. Ten separate calls would mean ten passes over
100 MB of pixels and ten rounding steps; this is one pass and one rounding
step, and adding another control to the chain costs nothing at run time.

Saturation and vibrance need all three channels at once, so they cannot go in
the table. They are folded into the same pass rather than run as a second
sweep, because at this size memory bandwidth costs more than arithmetic.

The shadow and highlight curves are shaped to stay monotonic at every setting:
a curve that inverts locally would show up as reversed contrast in the
midtones, which reads as a rendering bug rather than a strong edit. The test
suite checks monotonicity across the full parameter grid.

**Vibrance versus saturation.** Vibrance scales its effect by how unsaturated
the pixel already is, so a strong setting lifts flat colour and leaves an
already-vivid sky alone. That is the whole difference between the two.

`ria_apply_curve` takes explicit tables instead — one entry per input level
(256 or 65536 `uint16_t`), `NULL` for a channel to leave it unchanged. Use it
for a curve the parametric controls cannot express, such as one drawn by a
user or read from a profile. Alpha is never mapped.

## 10. Statistics

```c
ria_status ria_compute_histogram(const ria_image*, ria_histogram* out);
ria_status ria_auto_levels(ria_image*, float clip_percent);
ria_status ria_suggest_adjustments(const ria_image*, ria_adjustments* out);
```

`ria_histogram` holds 256 bins each for R, G, B and luma, the pixel count, and
the fraction of pixels clipped at black and at white — the numbers a
histogram display and a clipping warning need. 16-bit input is folded to 256
bins, since a per-level histogram of a 16-bit image is mostly zeroes and no
more informative to look at.

`ria_auto_levels` stretches **each channel** so that `clip_percent` of pixels
fall outside the range at each end, which also removes a colour cast. 0.5 is a
reasonable default. 0 stretches to the true extremes and is therefore
sensitive to a single hot pixel. It works at full precision rather than from
the 256-bin histogram, so a 16-bit image gets a 16-bit black point. A flat
image is returned unchanged rather than divided by zero.

`ria_suggest_adjustments` does not modify anything: it fills an
`ria_adjustments` with a starting point — black and white points from the
0.1% tails, and an exposure that aims the median at 0.45 — so a UI can show
the numbers before committing to them. The exposure correction is capped at
one stop either way, because beyond that the frame is deliberately high or low
key and "fixing" it is worse than leaving it alone.

## 11. Geometry

```c
ria_status ria_apply_orientation(const ria_image*, int flip, ria_image**);
ria_status ria_crop(const ria_image*, int x, int y, int w, int h, ria_image**);
ria_status ria_resize(const ria_image*, int w, int h, ria_resize_filter, ria_image**);
ria_status ria_fit_within(const ria_image*, int max_w, int max_h,
                          ria_resize_filter, ria_image**);
```

`ria_apply_orientation` rotates by a LibRaw flip code and clears
`pending_flip` on the result. Codes other than 0, 3, 5 and 6 copy unchanged
rather than guess — a guess here produces a silently sideways image.

`ria_crop` requires the rectangle to lie fully inside the source.

`ria_resize` offers two filters:

- `RIA_RESIZE_TRIANGLE` — a triangle filter whose support widens as the image
  shrinks: plain bilinear when enlarging, a correct area average when
  reducing. **This is the one you want.** A bilinear downscale of a 33 MP
  frame to a 400 px thumbnail reads four source pixels out of every ~7000 and
  turns fine detail into aliasing noise.
- `RIA_RESIZE_NEAREST` — fast and blocky.

Both are separable and run two passes with a 16-bit intermediate, so an 8-bit
resize does not round twice.

`ria_fit_within` scales to fit a box while preserving aspect ratio, and never
enlarges — thumbnail generation, in one call.

## 12. Spatial filters

```c
ria_status ria_gaussian_blur(ria_image*, float sigma);
ria_status ria_unsharp_mask(ria_image*, float sigma, float amount, float threshold);
```

Both operate in place and are separable, which is the only reason they are
usable at full resolution: a 2D Gaussian at sigma 2 needs a 13×13 window, 169
multiplies per pixel, while two 1D passes need 26.

`ria_unsharp_mask` computes `img + amount * (img - blur(img, sigma))` wherever
the local difference exceeds `threshold`, which is what keeps sharpening out
of flat sky and skin. The threshold is in normalised units (0–1), so it means
the same thing at 8 and 16 bits. A reasonable starting point for a 24 MP
frame is sigma 1.0, amount 0.6, threshold 0.01. Alpha is not sharpened.

Edges clamp to the last real pixel in both filters, so a blurred border does
not darken.

## 13. Output

```c
ria_status ria_write_pnm(const ria_image*, const char* path);
```

Binary PPM (P6) or PGM (P5), chosen by channel count; alpha is dropped, 16-bit
samples are written big-endian as the format requires. This is deliberately
the only writer: it is dependency-free and exists so that tests and CLI tools
can dump a result. Real output formats — JPEG, TIFF, PNG — belong to the
caller, who almost always has an encoder already.

## 14. The legacy ABI

`raw_images_api_legacy.h` declares the flat ABI that `raw_viewer`'s
`libraw_wrapper.so` exported before this library existed:

```c
RawImageResult* raw_decode_file(const char* path);
void            raw_free_result(RawImageResult*);
int             raw_read_meta(const char* path, RawImageMeta* out);
RawThumbResult* raw_decode_thumb(const char* path);
void            raw_free_thumb(RawThumbResult*);
int             raw_read_focus(const char* path, RawFocusPoint* out);
```

It exists so that an FFI binding written against the old wrapper keeps working
across the extraction without editing its struct mirrors. Every function is a
call into the `ria_*` API plus a struct copy; nothing is implemented twice.
Struct sizes are 32, 32, 156 and 40 bytes, unchanged.

**Do not build new work on it.** It has no error reporting, no 16-bit output,
no processing, and its structs cannot change without breaking the callers it
exists to serve. Turn it off with `-DRIA_BUILD_LEGACY_ABI=OFF`.

## 15. Performance

Measured with `ria_tool bench` on an 8-core x86-64 desktop, LibRaw 0.22.2,
release build with OpenMP. Full-resolution 8-bit RGB output.

| operation | Nikon Z 6_2 NEF, 24 MP | Canon EOS R7 CR3, 33 MP |
|---|---|---|
| open (headers + MakerNotes) | 1.3 ms | 1.7 ms |
| metadata | < 0.1 ms | < 0.1 ms |
| focus point | < 0.1 ms | < 0.1 ms |
| embedded preview | 2.9 ms | 6.7 ms |
| **decode, full** | **1528 ms** | **1429 ms** |
| decode, `half_size` | 970 ms | 545 ms |
| adjustments (whole struct) | 95 ms | 116 ms |
| histogram | 38 ms | 51 ms |
| resize to 800 px | 81 ms | 101 ms |
| unsharp mask, sigma 1.0 | 613 ms | 835 ms |

What the numbers say:

- **Reading facts is free.** Anything that does not touch sensor data is
  under 10 ms. A file browser can show metadata and focus points for a whole
  directory without a progress bar.
- **The preview is 200–500x cheaper than the decode** and nearly as large.
  For judging focus and composition it is enough on its own.
- **The decode is the demosaic**, and the demosaic is memory-bandwidth-bound.
  The algorithm choice matters; the thread count does not, much.
- `half_size` helps Canon more than Nikon because Nikon's unpack step is
  single-threaded inside LibRaw, so a larger share of the remaining time is
  serial.
- **Editing is cheap next to decoding.** A full tonal edit costs less than a
  tenth of the decode, which is what makes interactive sliders on a
  full-resolution frame practical without a downsampled proxy.
- Unsharp is the exception: it is two full convolution passes plus a copy of
  the image. Sharpen after resizing, not before, whenever the output is
  smaller than the frame.

## 16. Binding from another language

The ABI was shaped for this. Everything crosses as a pointer or an `int`; no
struct is passed or returned by value; no callbacks, no varargs, no bitfields;
`ria_status` is a plain `int` enum.

To mirror a struct, copy the field order from `raw_images_api.h` exactly and
check the total size against `sizeof` from C — a field inserted mid-struct or
an `int32` where C has a pointer produces plausible garbage rather than a
crash, and a size assertion at startup is the cheapest guard there is.
`raw_viewer`'s `tool/ffi_check.dart` is a worked example of exactly that.

Watch for:

- **`ria_image.stride` and `data_size` are `size_t`** — 64-bit on LP64, not
  `int`. The four `int` fields before them are 32-bit.
- **`ria_metadata` contains fixed-size `char` arrays**, not pointers, and is
  filled by the callee. Allocate it on your side.
- **`ria_metadata.timestamp` is `int64_t`**, not `time_t`.
- **Free with the matching function**, not your language's allocator. The
  pixel buffer inside an `ria_image` is not separately owned.

## 17. Extending the library

The seams that new work is expected to arrive on:

**A new per-pixel tonal control** goes in `ria_adjustments` (at the end of the
struct) and in `transfer()` in `src/ria_adjust.c`, if it acts on one channel
at a time. It then costs nothing at run time — it is folded into the lookup
table with everything else. If it needs neighbouring pixels or all three
channels, it is a new function instead.

**A new spatial filter** belongs in `src/ria_filter.c`. Follow the existing
shape: separable if at all possible, row-parallel with `RIA_PARALLEL_ROWS`,
edges clamped, and one variant per sample type through the
`RIA_DEFINE_CONVOLVE`-style macro rather than a bit-depth test inside the
loop — the compiler cannot hoist that test, because nothing tells it that the
source and destination do not alias.

**A new output format** belongs in a new translation unit with its own CMake
option, following how `RIA_WITH_JPEG` gates `ria_preview_to_image`: absent
dependency means the function returns `RIA_ERR_UNSUPPORTED`, never a silently
wrong result.

**Anything needing LibRaw internals** can start behind
`ria_raw_native_handle` and graduate into a wrapped call once its shape is
settled.

Two rules that are not negotiable, because breaking either produces silent
corruption in a mirrored struct somewhere: **public structs are append-only**,
and **nothing is passed or returned by value**.

The test binary is the place to establish that a new operation is correct.
`tests/test_ria.c` is written against properties that must hold — a rotation
composed with its inverse is the identity, a resize of a flat field is flat, a
tone curve is monotonic — rather than golden values, so the tests keep their
meaning when an implementation is replaced.
