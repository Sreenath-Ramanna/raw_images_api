# raw_images_api

Camera RAW decoding and image enhancement, in C.

A library over [LibRaw](https://www.libraw.org/) that reads RAW files —
metadata, the camera's embedded preview, the autofocus point, the demosaiced
image, the bare sensor mosaic — and a set of processing operations to work on
the result: exposure and colour adjustment, auto levels, histograms, resize,
rotation, blur and sharpening.

Extracted from [raw_viewer](../raw_viewer), which is now one of its consumers.

**[API.md](API.md) is the reference.** This file is how to build it.

## Why C

Decoding a 33 MP frame is ~1.5 s of demosaic and ~130 MB of pixels; a full
tonal edit touches 100 million samples. That work belongs close to the metal,
and a flat C ABI is what every other language can call — this library is used
from Dart today by way of `dart:ffi`, with nothing in between.

## Requirements

- LibRaw development headers (0.20 or newer; developed against 0.22.2)
- CMake 3.13+, a C11 compiler
- OpenMP, optionally — the pixel loops parallelise across rows when it is
  available, and run serially when it is not

```bash
sudo dnf install LibRaw-devel cmake ninja-build          # Fedora
sudo apt-get install libraw-dev cmake ninja-build        # Debian / Ubuntu
```

> On Fedora the package is `LibRaw-devel`, capitalised. A lowercase
> `libraw-devel` does not exist, and `libraw1394-devel` is an unrelated
> FireWire library that installs cleanly and does not help.

## Build

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
./build/ria_tests                     # 208 checks, no camera files needed
sudo cmake --install build            # optional
```

Pass RAW files to the test binary to exercise the decode paths against real
data as well:

```bash
./build/ria_tests ~/photos/*.NEF ~/photos/*.CR3
```

## Try it

`ria_tool` exercises the whole API from the command line and doubles as the
worked example — every call for reading a file appears in it once.

```bash
./build/ria_tool info    photo.CR3
./build/ria_tool preview photo.CR3 preview.jpg
./build/ria_tool bench   photo.CR3
./build/ria_tool decode  photo.CR3 out.ppm --auto-levels --vibrance 0.3 \
                                           --sharpen 0.8 --fit 2048
```

`info` prints camera, lens, exposure, sensor levels, white balance, the
resolved autofocus point and the embedded preview's size. `bench` times every
stage separately, which is the fastest way to see where a slow file is
spending its time.

## Use it

```cmake
find_package(raw_images_api REQUIRED)
target_link_libraries(myapp PRIVATE raw_images_api::raw_images_api)
```

or `pkg-config --cflags --libs raw_images_api`, or embed the source tree with
`add_subdirectory` — see [API.md §3](API.md#3-building-and-linking).

```c
#include <raw_images_api.h>

ria_image* img;
if (ria_decode_file("photo.CR3", NULL, &img) == RIA_OK) {
    ria_auto_levels(img, 0.5f);
    ria_unsharp_mask(img, 1.0f, 0.6f, 0.01f);
    ria_write_pnm(img, "out.ppm");
    ria_image_free(img);
}
```

## Layout

```
include/raw_images_api.h         the public API — one header
include/raw_images_api_legacy.h  the pre-extraction raw_* ABI, kept for raw_viewer
src/ria_raw.c                    opening files, decoding, previews, metadata
src/ria_focus.c                  Canon and Nikon autofocus MakerNotes
src/ria_image.c                  buffers, format conversion, geometry, PNM
src/ria_adjust.c                 tonal and colour editing, histograms
src/ria_filter.c                 blur and unsharp mask
src/ria_legacy.c                 the legacy ABI, implemented over the ria_* API
examples/ria_tool.c              command-line exercise of everything
tests/test_ria.c                 property-based tests, plus an optional file pass
```

## Supported formats

Anything LibRaw can open. `ria_is_raw_extension` recognises Canon `.cr2`
`.cr3` `.crw`, Nikon `.nef` `.nrw`, Sony `.arw` `.srf` `.sr2`, Fujifilm
`.raf`, Olympus `.orf`, Pentax `.pef`, Adobe `.dng`, Panasonic `.rw2`, Leica
`.rwl`, Phase One `.iiq`, Hasselblad `.3fr` `.fff`, Epson `.erf`, Minolta
`.mrw`, Sigma `.x3f` and generic `.raw`.

Verified end to end against Nikon Z 6_2 (NEF) and Canon EOS R7 (CR3) files.
The autofocus parsing is specific to those two vendors; everything else is
vendor-neutral.

## Roadmap

[approach.md](approach.md) is the design for colour temperature reading and
setting, EV-binned zone histograms, per-zone exposure control, and
exposure/contrast in stops. [PLAN.md](PLAN.md) sequences that work.

Both turn on one point: those operations are only meaningful on
**scene-referred linear** data, and the current decode is display-referred —
it clips 1.4–2.3 % of a frame and applies a scene-dependent +1.7 EV before any
control is touched. `ria_adjustments.exposure_ev` is defective for that
reason and is removed rather than patched.

## Status

Version 0.1.0. The API is young and the struct layouts are append-only from
here, but function signatures may still change before 1.0. The legacy `raw_*`
ABI is frozen by definition.
