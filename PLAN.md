# Plan — colour temperature and EV-based tone control

Five features, in the order they were asked for:

1. Read the colour temperature recorded in the RAW file.
2. Adjust decoded RGB to a different colour temperature.
3. Histograms in luminance and per-channel modes, plus a zone
   (black / shadow / highlight / white) representation.
4. Adjust pixels in a zone by an exposure value, −3 EV to +3 EV.
5. Adjust brightness and contrast by EV, −3 EV to +3 EV.

Two of these are partly built already, one has a correctness problem underneath
it that has to be fixed first, and one needs a design decision that the
question itself raises. Details below.

---

## 0. Does this already exist?

Short answer: **the algorithms all exist in open source, but not in a form this
library can call.** The two mature implementations — darktable and RawTherapee
— are GPL applications, not libraries: the code is entangled with their
pipelines, their GUIs and their own image structures. Lifting it would mean
either vendoring GPL code (which relicenses this library) or reimplementing
from the same published maths, which is what everyone does.

Feature by feature:

| # | Feature | Prior art | Reusable here? |
|---|---|---|---|
| 1 | CCT from a RAW file | darktable `temperature.c` / `colorin`; RawTherapee `ColorTemp::mul2temp`; Adobe DNG SDK `dng_temperature` | **No** — GPL applications. LibRaw gives the *inputs*, not the answer |
| 2 | Re-balance to a target CCT | RawTherapee `ColorTemp::temp2mul`; darktable white balance / color calibration; lcms2 for the colorimetry | **Partly** — lcms2 (MIT) has the CCT↔white-point half |
| 3 | Histogram | Everywhere | **Already built here** — see below |
| 3b | Zone representation | Zone System; Lightroom's Blacks/Shadows/Highlights/Whites; darktable `tone equalizer` (9 zones spaced 1 EV) | Concept yes, code no |
| 4 | Per-zone EV adjustment | darktable `tone equalizer`, `gegl:shadows-highlights` | Concept yes, code no |
| 5 | Exposure in EV | darktable `exposure` iop | Trivial to implement; the subtlety is elsewhere (§1) |

Things worth knowing before designing anything:

**LibRaw will not give you a Kelvin value.** It exposes the raw materials —
`color.cam_mul` (as-shot multipliers), `color.cam_xyz` (camera→XYZ matrix),
and, *when the camera writes them*, `color.WBCT_Coeffs[64][5]` (the camera's
own Kelvin→multiplier calibration curve) and `color.WB_Coeffs[256][4]` (named
presets). LibRaw's own position is that white balance is not measured in
Kelvin and that the reliable route is a table of CT against coefficients,
interpolated. That is exactly what we will do where a table exists.

**Whether a table exists is vendor-dependent, and we can already measure it.**
Probing the test images in `raw_viewer/test-images`:

| Camera | `WBCT_Coeffs` rows | `WB_Coeffs` presets | Route available |
|---|---|---|---|
| Canon EOS R7 (CR3) | **15** (3200 K – 10900 K) | 9 (Daylight, Shade, Cloudy, Tungsten, Flash, Auto, Measured …) | camera table **and** colorimetric |
| Nikon Z 6_2 (NEF) | **0** | 0 | colorimetric only |

So the design needs two paths, not one, and must say which it used.

**A single CCT does not describe a white balance.** The R7 file's as-shot
multipliers normalise to R/G = 1.921, B/G = 1.568. Read against that camera's
own table, the red ratio implies *above* 6000 K while the blue ratio implies
*below* 5600 K. They disagree because the shot's white point sits off the
Planckian locus — which is what "tint" measures. Any API returning only a
Kelvin number would be discarding the disagreement rather than reporting it.
darktable handles this by labelling a reported CCT "(daylight)", "(black
body)" or "(invalid)" depending on how far off-locus the illuminant is, and
notes that some Olympus and Sony bodies are *always* invalid by that test. We
should copy that honesty, not just the formula.

**lcms2 is the one directly reusable piece.** MIT licensed, already installed
here (2.16), and `cmsTempFromWhitePoint` / `cmsWhitePointFromTemp` implement
Robertson's method over CIE xyY. **Recommendation: do not take the runtime
dependency.** The part we need is ~60 lines of table-driven arithmetic, and
this library's selling point is that it needs LibRaw and nothing else. Use
lcms2 in the *test binary* instead, as an independent oracle for our own
implementation — a dependency in tests costs nothing and catches real errors.

### What is already built

Feature 3's first half is done. `ria_compute_histogram` already returns 256
bins for **each of R, G, B and luma**, plus the pixel count and the clipped
fractions, at 38–51 ms for a 24–33 MP frame. The staging suggested in the
request — luminance now, per-channel later — is unnecessary; per-channel is
already there and costs nothing extra worth deferring for.

What is genuinely missing from feature 3 is the *zone* view, and the reason it
is missing is interesting enough to have its own section (§4).

Feature 5's brightness half also exists as `ria_adjustments.exposure_ev` — but
it is wrong, which is the subject of the next section.

---

## 1. Prerequisite: a linear working space

**This blocks features 2, 4 and 5 and must land first.**

An exposure value is a ratio of *linear* light. `+1 EV` means "twice as many
photons". `ria_raw_decode` returns display-encoded pixels — LibRaw applies an
sRGB-like curve, `gamma_power` 2.222 with a 4.5 toe, by default — and the
existing `ria_apply_adjustments` multiplies **those encoded values** by
`2^EV`.

That is not an exposure adjustment. Working it through for encoded 0.5 with a
2.222 gamma:

| | linear | encoded (8-bit) |
|---|---|---|
| input | 0.214 | 128 |
| +1 EV, correct | 0.429 | **174** |
| +1 EV, as implemented | — | **255** (clipped) |

The error is over a stop at the top of the range, and it grows with the input
value. `tests/test_ria.c` currently *asserts* the wrong behaviour
(`+1 EV on 64 gives 128`), so the test suite is pinning the defect in place.
Every feature below is specified in EV, so all of them inherit this unless it
is fixed first.

### Fix

Give `ria_image` two new fields — the struct is append-only, so this is
compatible:

```c
typedef enum {
    RIA_TRANSFER_LINEAR = 0,
    RIA_TRANSFER_SRGB,          /* the IEC piecewise curve                */
    RIA_TRANSFER_GAMMA,         /* power + toe slope, LibRaw's gamm[0..1] */
} ria_transfer;

/* appended to ria_image */
ria_transfer   transfer;
float          transfer_gamma, transfer_slope;
ria_colorspace colorspace;      /* which primaries `data` is in */
```

`ria_raw_decode` fills these from the options it was given, so a decoded image
knows what it is. `ria_image_new` defaults to sRGB, which is what a caller
wrapping a screenshot means.

Then add the conversion, and — the point of the whole exercise —
**fold it into the lookup table that already exists**:

```c
ria_status ria_image_to_linear(ria_image*);   /* usually not needed directly */
ria_status ria_image_to_transfer(ria_image*, ria_transfer, float gamma, float slope);
```

`ria_apply_adjustments` already collapses its per-channel chain into one LUT
per channel. Linearise-on-entry and re-encode-on-exit are two more stages in
that chain, so correctness here costs **two extra operations per LUT entry,
not per pixel** — 512 extra `powf` calls on an image with 100 million samples.
There is no performance argument against doing this properly.

### Fallout to handle

- `ria_adjustments.exposure_ev` changes meaning. It becomes correct, and
  images processed with the old behaviour will look different. Given the
  library is at 0.1.0 with one consumer that does not use adjustments at all,
  **change it rather than adding a parallel correct version.** Note it in the
  changelog as a behavioural fix.
- The corresponding test assertions must be rewritten to the linear-light
  values, and a comment must say why 174 rather than 255, or someone will
  "fix" it back.
- `black_point` / `white_point` / `contrast` stay defined in *display* space,
  because that is where they are perceptually meaningful. Document which
  controls act in which space — a table in API.md §8.
- 8-bit round trips through linear lose precision in the shadows. Recommend
  16-bit (`output_bits = 16`) for anything applying more than ~1 EV, and say
  so in the docs. The LUT is exact for 16-bit input; it is the 8-bit
  *quantisation* that costs, not the arithmetic.

**Effort: ~250 lines plus test rework. One sitting.**

---

## 2. Feature 1 — the recorded colour temperature

### API

```c
typedef enum {
    RIA_WB_SOURCE_NONE = 0,
    RIA_WB_SOURCE_CAMERA_TABLE,   /* interpolated in the camera's own curve */
    RIA_WB_SOURCE_COLORIMETRIC,   /* computed through cam_xyz              */
} ria_wb_source;

typedef enum {
    RIA_WB_LOCUS_PLANCKIAN = 0,   /* CCT is meaningful                     */
    RIA_WB_LOCUS_DAYLIGHT,        /* CCT is meaningful                     */
    RIA_WB_LOCUS_OFF,             /* too far off-locus; CCT is decorative  */
} ria_wb_locus;

typedef struct {
    float         cct_kelvin;     /* 0 when unknown                        */
    float         tint;           /* green-magenta, 1.0 neutral            */
    float         duv;            /* signed distance from the locus        */
    float         mul[4];         /* the multipliers this describes        */
    float         white_xy[2];    /* the white point, in CIE xy            */
    ria_wb_source source;
    ria_wb_locus  locus;
    int           reliable;       /* 0 => report the number with a caveat  */
    int           camera_preset;  /* LibRaw WB illuminant enum, -1 if none */
} ria_white_balance;

ria_status ria_raw_white_balance(ria_raw*, ria_white_balance* out);
```

Deliberately **not** added to `ria_metadata`: it is a derived quantity with a
reliability flag, and a caller should have to look at `reliable` rather than
find a Kelvin number sitting innocently next to the ISO.

### Algorithm

Normalise the as-shot multipliers by green first — Canon writes them as
1024-based integers, Nikon as floats around 1.0, and only the ratios matter.

**Path A — the camera's own table** (when `WBCT_Coeffs` has ≥ 2 rows):

1. Reparameterise the table by **mired** (10⁶/K) rather than Kelvin. Colour
   temperature is perceptually and numerically far closer to uniform in
   mired: the R7's table steps 3200→3500→3800 K at the bottom and
   10000→10900 K at the top, which is roughly even in mired and wildly uneven
   in Kelvin. Interpolating in Kelvin would bias every result warm.
2. Search for the mired value whose interpolated (R/G, B/G) is closest to the
   measured pair, in log space, minimising squared error. One-dimensional,
   monotonic, ~20 golden-section iterations — microseconds.
3. The residual perpendicular to the curve **is the tint**. Report it rather
   than discarding it; this is the R7 disagreement described in §0.
4. `source = CAMERA_TABLE`. Interpolating within the table's range is
   reliable; extrapolating beyond its endpoints is not — clamp and set
   `reliable = 0` outside.

**Path B — colorimetric** (always available, and the only option for Nikon):

1. The camera-space neutral is `(1/mul_r, 1/mul_g, 1/mul_b)`.
2. `XYZ = cam_xyz⁻¹ · neutral`. `cam_xyz` is always populated by LibRaw from
   its own camera database. (3×3 inversion, ~20 lines.)
3. `XYZ → xy`, then **xy → CCT by Robertson's method**: 31 isotemperature
   lines, walk until the sign of the cross product flips, interpolate. The
   table is standard published data.
4. `Duv` — signed distance from the Planckian locus — falls out of the same
   calculation and drives `locus` and `reliable`. Threshold: |Duv| > 0.05 is
   `RIA_WB_LOCUS_OFF`, matching darktable's ±0.5 % spectral criterion closely
   enough for a flag.
5. Also test against the CIE **D** (daylight) locus above 4000 K, and report
   `RIA_WB_LOCUS_DAYLIGHT` when it fits better. Below 4000 K there is no
   daylight reference and only the black-body locus applies — the same split
   RawTherapee makes.

**Choosing:** prefer Path A when the table exists and the answer lands inside
it, because it reproduces what the camera itself would display. Fall back to
Path B. Always report which was used — the two will not agree exactly, and a
caller comparing values across a mixed shoot needs to know why.

### Testing

- **Cross-check the two paths on Canon files**, where both are available.
  They should agree within a few hundred Kelvin; a larger gap means one of
  them is wrong. This is the single most valuable test here and it needs no
  external tool.
- **Validate xy→CCT against lcms2** (`cmsTempFromWhitePoint`) in the test
  binary, guarded by `find_package(lcms2)` so the suite still builds without
  it.
- Round-trip against feature 2: `cct → mul → cct` within 1 %.
- Known illuminants: D65 must give ≈ 6504 K with Duv ≈ 0, D50 ≈ 5003 K,
  Illuminant A ≈ 2856 K.
- **Install `exiftool` for ground truth.** Canon writes a `ColorTemperature`
  MakerNote tag holding the value the camera itself decided on; comparing our
  Path A result against it is the strongest available check. It is not
  installed on this machine, and it should be before this feature is called
  done.

**Effort: ~400 lines (Robertson table, matrix inversion, table search) plus
tests. Two sittings, most of it in validation rather than code.**

---

## 3. Feature 2 — re-balancing to a target temperature

There are two ways to do this and they are not equivalent. Both belong in the
API, clearly labelled, because the right one depends on whether the caller is
producing a final image or dragging a slider.

### Mode A — re-decode with new multipliers (correct)

White balance belongs on **linear sensor data, before demosaic**. That is
where LibRaw applies it, and it is the only place it can be applied without
clipping artefacts or hue shifts.

```c
/* appended to ria_decode_options */
float target_cct;    /* Kelvin; 0 = as shot          */
float target_tint;   /* 1.0 = neutral                */
```

Implementation is small: convert (CCT, tint) → multipliers by inverting
feature 1's path A or B, write them into `lr->params.user_mul[]`, clear
`use_camera_wb`, decode. The handle already re-reads the file when a decode
has consumed it, so "show me this at 5200 K instead" is one call.

Cost: a full re-decode, 1.5–3 s. Correct, full precision, no clipping
surprises.

### Mode B — chromatic adaptation of decoded RGB (fast, approximate)

For a slider, 1.5 s per frame is unusable. The post-hoc path:

1. Linearise (§1).
2. Working RGB → XYZ, using the primaries of `image->colorspace`.
3. **Bradford chromatic adaptation** from the source white point to the
   target. This is the standard von Kries-style transform: XYZ → LMS via the
   Bradford matrix, per-channel scale by the white point ratio, back.
4. XYZ → working RGB.
5. Re-encode.

Steps 2–4 collapse into **one 3×3 matrix**, computed once. Steps 1 and 5 are
LUTs. So the per-pixel cost is 9 multiplies plus two lookups — comparable to
the existing adjustment pass, ~120 ms on 33 MP.

```c
ria_status ria_white_balance_matrix(float from_cct, float from_tint,
                                    float to_cct, float to_tint,
                                    ria_colorspace, float matrix[9]);
ria_status ria_apply_color_matrix(ria_image*, const float matrix[9]);
ria_status ria_apply_temperature(ria_image*, float from_cct, float from_tint,
                                 float to_cct, float to_tint);
```

Exposing `ria_apply_color_matrix` separately is worth it: an arbitrary 3×3 in
linear light is also how you do colour space conversion, a colour cast fix, or
a channel mixer. One primitive, several features.

**Document the limits honestly.** Highlights already clipped by the decode
cannot be recovered — a strong warm-to-cool move will show it as a hue shift
in the clipped regions, because the three channels clipped at different scene
luminances and the adaptation cannot know that. A ±1000 K nudge is fine; a
3000 K correction should be done in Mode A. Suggested UI pattern, worth
putting in the docs: adapt for the preview, re-decode for the export.

**Effort: ~350 lines across both modes plus tests. Two sittings.**

---

## 4. Feature 3 — histogram modes and the zone representation

### Modes

Per-channel is already implemented, so the work here is only to let a caller
*skip* it:

```c
typedef enum {
    RIA_HIST_LUMA     = 1 << 0,
    RIA_HIST_RGB      = 1 << 1,
    RIA_HIST_ALL      = RIA_HIST_LUMA | RIA_HIST_RGB,
} ria_histogram_mode;

ria_status ria_compute_histogram_ex(const ria_image*, ria_histogram_mode,
                                    ria_histogram* out);
```

`ria_compute_histogram` stays as the `RIA_HIST_ALL` shorthand, so nothing
breaks. Luma-only saves roughly 40 % of the pass. Small, an hour's work.

### The zone representation — the part that needs a decision

The existing `ria_histogram` is **256 bins of display-encoded value**. It is
the right structure for drawing a histogram widget and the wrong one for
exposure work, for two reasons:

1. **The bins are not in EV.** Feature 4 adjusts by stops, so the natural axis
   is `log2` of linear luminance. In a 256-bin encoded histogram the darkest
   four stops of a scene are crammed into roughly the bottom 40 bins, so any
   zone boundary placed there lands with terrible resolution.
2. **256 linear bins are not a partition anyone means.** "Shadows" is not
   "bins 32–96". It is a soft region, and the softness matters — see below.

So: **add a second structure, do not change the first.** They answer different
questions.

```c
#define RIA_ZONE_BINS 48           /* -10 EV .. +2 EV in 0.25 EV steps */

typedef struct {
    uint32_t bins[RIA_ZONE_BINS];  /* by log2 luminance, relative to white */
    float    ev_min, ev_max;       /* the range the bins span              */
    uint64_t pixels;
    double   mean_ev, median_ev;
    double   below_range, above_range;  /* fractions falling off each end   */
} ria_zone_histogram;

ria_status ria_compute_zone_histogram(const ria_image*, ria_zone_histogram*);
```

0.25 EV bins over 12 stops is 48 bins — finer than darktable's tone equalizer,
which works in 9 zones spaced 1 EV, and coarse enough to stay noise-free on a
24 MP frame.

### Black / shadow / highlight / white

The request asks for these four as an alternative representation. The design
question is *how the boundaries behave*, and getting it wrong here is the
single most likely way for feature 4 to produce visibly broken images.

**Do not use hard thresholds.** If "shadow" is `EV < −4` and "highlight" is
`EV ≥ −4`, then lifting shadows by +2 EV and leaving highlights alone puts a
2-stop cliff at exactly −4 EV. In a smooth gradient — a sky, a wall, skin —
that renders as a hard visible edge where no edge exists in the scene. It is
the classic posterisation failure of naive zone tools.

**Use a partition of unity.** Define four weight functions over EV that are
smooth and **sum to exactly 1 at every EV**. Then:

- the *analysis* number ("how much of this image is shadow") is
  `Σ_bins hist[b] · w_shadow(EV(b)) / total`, and
- the *adjustment* in feature 4 uses **the same weights**,

so the two can never disagree about what a shadow is. That shared definition
is the reason to do it this way rather than with thresholds.

With knots `k₀ < k₁ < k₂ < k₃` in EV relative to white (defaults **−8, −4,
−1.5, 0**), and `s(t) = 3t² − 2t³` (smoothstep) as the ramp:

```
w_black(e)     = 1                          e ≤ k₀
               = 1 − s((e−k₀)/(k₁−k₀))      k₀ < e < k₁
               = 0                          otherwise

w_shadow(e)    = s((e−k₀)/(k₁−k₀))          k₀ < e < k₁
               = 1 − s((e−k₁)/(k₂−k₁))      k₁ ≤ e < k₂
               = 0                          otherwise

w_highlight(e) = s((e−k₁)/(k₂−k₁))          k₁ ≤ e < k₂
               = 1 − s((e−k₂)/(k₃−k₂))      k₂ ≤ e < k₃
               = 0                          otherwise

w_white(e)     = s((e−k₂)/(k₃−k₂))          k₂ ≤ e < k₃
               = 1                          e ≥ k₃
               = 0                          otherwise
```

`s(t) + s(1−t) = 1` identically, so the four sum to 1 everywhere — provable,
and a one-line test. Smoothstep rather than a linear ramp because it is C¹:
the *slope* is continuous at the knots too, so a strong adjustment does not
leave a visible crease where a linear ramp's derivative jumps.

```c
typedef struct {
    double black, shadow, highlight, white;  /* fractions, sum to 1        */
    double mean_ev, median_ev;
    double clipped_black, clipped_white;     /* true clipping, not zones   */
    float  knots_ev[4];                      /* what produced these        */
} ria_zone_summary;

ria_status ria_zone_summary_from(const ria_zone_histogram*,
                                 const float knots_ev[4] /* NULL = default */,
                                 ria_zone_summary* out);
void       ria_zone_weights(float ev, const float knots_ev[4], float w[4]);
```

`ria_zone_weights` is public on purpose: it is the contract between analysis
and adjustment, and a caller drawing a zone overlay needs the same numbers.

**The knot defaults need calibration against real images**, not just
reasoning. −8/−4/−1.5/0 is a starting point taken from where Lightroom's
regions roughly sit; the plan should include a session of looking at zone
summaries for a spread of the test files and checking that "this is a shadowy
image" reads as a high `shadow` fraction. Budget for moving them once.

**Effort: ~250 lines plus tests, plus a calibration session.**

---

## 5. Features 4 and 5 — one EV tone engine

These are specified separately but they are the same operation, and building
them separately would mean two passes over the pixels and two rounding steps
for what is mathematically one curve.

**Every one of them is a function from input EV to output EV, applied as a
luminance-preserving gain.** Zones, brightness and contrast are three
contributions to a single curve — exactly as `ria_adjustments` already folds
nine controls into one LUT.

```c
typedef struct {
    float brightness_ev;      /* -3 .. +3, linear-light scale            */
    float contrast_ev;        /* -3 .. +3, see below                     */
    float contrast_pivot;     /* linear luminance, default 0.18          */

    float black_ev;           /* -3 .. +3, per zone                      */
    float shadow_ev;
    float highlight_ev;
    float white_ev;

    float knots_ev[4];        /* zone knots; all zero = library default  */
    int   soft_limit;         /* 1 = scale back to stay monotonic        */
} ria_tone_ev;

void       ria_tone_ev_defaults(ria_tone_ev*);
ria_status ria_apply_tone_ev(ria_image*, const ria_tone_ev*);
float      ria_tone_ev_min_slope(const ria_tone_ev*);  /* < 0 => inverts */
```

### The curve

With `e = log2(Y / Y_white)` for pixel luminance `Y`:

```
e₁ = e + brightness_ev + Σₖ wₖ(e) · zoneₖ_ev        (exposure-like terms)
e₂ = p + slope · (e₁ − p)                            (contrast about pivot p)

slope = 2^(contrast_ev / 3)
gain  = 2^(e₂ − e)
```

Then `R,G,B ← R,G,B × gain`, **all three by the same factor**. Scaling
channels independently would shift hue; scaling uniformly changes luminance
and leaves the chromaticity alone, which is what "adjust the exposure of the
shadows" means.

Two definitional points, because neither is standard:

**Contrast in EV is not a standard unit** — contrast is normally a unitless
slope. The definition above is the one worth adopting: `slope = 2^(c/3)`, so

- `c = 0` → slope 1, no change;
- `c = +3` → slope 2, **the number of stops between any two tones doubles**;
- `c = −3` → slope 0.5, halves it;
- `c` and `−c` compose to the identity — which is a property a test can assert.

It never degenerates (an additive definition like `(3+c)/3` collapses to a
flat grey at `c = −3`), and it is symmetric in the log domain where contrast
actually lives. Say all this in the docs, because a user typing "+2" deserves
to know what it means.

**Zone weights are evaluated on the input EV**, `e`, not on `e₁`. Evaluating
them on the shifted value would make the equation implicit and need iteration
per pixel for no visible benefit. Worth one line of comment where it happens,
or someone will "fix" it later.

### Implementation

The gain depends only on luminance, so it is **a one-dimensional function** —
tabulate it. Precompute `gain_lut[]` indexed by quantised luminance (4096
entries for 8-bit input, 65536 for 16-bit), then per pixel:

1. `Y = 0.2126R + 0.7152G + 0.0722B` on **linearised** values (the
   linearisation folds into the same LUT — §1);
2. one lookup;
3. three multiplies;
4. re-encode through the output LUT.

About 10 operations per pixel, one pass, no `powf`, no `log2` in the hot loop.
**Estimated 120–160 ms for 33 MP**, in line with the existing adjustment pass
at 116 ms. Row-parallel with `RIA_PARALLEL_ROWS` like everything else.

### The thing that will bite: non-monotonicity

Adjacent zones pushed in opposite directions can invert the tone curve. The
slope is

```
d e₂ / d e = slope · (1 + Σₖ wₖ′(e) · zoneₖ_ev)
```

and smoothstep has `|s′|max = 1.5 / span`. Between the `k₂ = −1.5` and
`k₃ = 0` knots the span is only 1.5 EV, so a 6 EV difference between
`highlight_ev` and `white_ev` gives `1 + 6 × 1.5/1.5 = −5`. **Strongly
negative: local contrast inverts, and the image looks solarised.** This is
reachable well inside the requested ±3 EV range, so it is not a theoretical
concern.

Three things follow:

1. `ria_tone_ev_min_slope()` computes the worst slope over the curve, so a
   caller can grey out or warn. Cheap — evaluate over the 48 zone bins.
2. `soft_limit` scales all four zone deltas by a common factor until the
   minimum slope reaches a floor (0.05 is a reasonable target). A common
   factor rather than per-zone clamping preserves the *shape* the user asked
   for while making it realisable.
3. **Reconsider the knot spacing.** `−1.5 → 0` being the tightest gap is what
   makes the highlight/white pair the fragile one. Widening it, or making the
   weight ramps wider than the knot spacing (overlapping basis functions
   rather than adjacent ones), buys headroom. Settle this during the
   calibration session in §4.

### Relationship to the existing controls

`ria_adjustments` already has `exposure_ev`, `contrast`, `shadows` and
`highlights`. Overlap has to be resolved rather than left to accumulate:

| existing | fate |
|---|---|
| `exposure_ev` | **fixed** by §1 to work in linear light; equals `ria_tone_ev.brightness_ev` |
| `contrast` (unitless, display space, 0.5 pivot) | keep — it is a different, perceptually-anchored control. Document the difference from `contrast_ev` |
| `shadows`, `highlights` (display-space weights) | **supersede** with the zone system, which is strictly more capable. Keep them for one release with a doc note, remove at 0.3 |
| `black_point`, `white_point` | keep, display space, unrelated |

Do not let both systems drift. API.md should gain a short table saying which
control works in which domain, and why.

**Effort: ~300 lines plus tests. Two sittings, one of which is the
monotonicity work.**

---

## 6. Sequencing

| Phase | Contents | Blocks | Effort |
|---|---|---|---|
| **A** | Linear working space; `ria_image.transfer`/`colorspace`; fix `exposure_ev`; rework the tests it broke | everything below | 1 |
| **B** | Feature 1 — `ria_raw_white_balance`, Robertson, camera-table path | C | 2 |
| **C** | Feature 2 Mode A — `target_cct` in decode options | — | 0.5 |
| **D** | Feature 3 — histogram modes, zone histogram, zone summary, weights | E | 1.5 |
| **E** | Features 4 + 5 — the EV tone engine | — | 2 |
| **F** | Feature 2 Mode B — chromatic adaptation on decoded RGB | needs A | 1.5 |

(Effort in rough sittings, not days.)

A is non-negotiably first. B before C only because C needs the CCT→multiplier
conversion that B builds. D before E for the same reason — E consumes D's
weight functions. F is last because Mode A already delivers the feature
correctly, and Mode B is a performance optimisation for interactive use.

**Ship A+B+C as 0.2.0** — that is "read and set colour temperature", a
coherent release. **D+E as 0.3.0** — "zone and EV tone control". F folds into
whichever is convenient.

## 7. Deferred

- **Spatially-aware zone adjustment.** The v1 engine maps each pixel by its
  own luminance, which preserves hue and produces no halos, but it flattens
  local contrast at strong settings: a dark object against a bright background
  gets lifted as much as a genuinely shadowed region. darktable's tone
  equalizer solves this with a **guided filter** over a blurred luminance mask,
  so the adjustment follows regions rather than pixels. That is the right v2,
  and it is a substantial piece of work — a guided filter is its own
  implementation and its own performance problem. Explicitly out of scope
  here; the v1 API does not need to change to accommodate it later, since it
  is an internal change to how the mask luminance is computed.
- **Per-channel zone adjustment** (split toning: warm highlights, cool
  shadows). Falls out almost free once the zone weights exist — three gains
  instead of one — but it is a different feature and should be asked for.
- **Tint as a first-class control** in Mode A. Feature 1 reports it; setting
  it needs the same table inversion with one more degree of freedom.
- **A real tone curve editor** (`ria_apply_curve` already accepts arbitrary
  LUTs; a spline-fitting helper on top would make it usable).

## 8. Risks

- **CCT is not always meaningful and the API must not pretend otherwise.**
  The `reliable` / `locus` flags are load-bearing, not decoration. A caller
  that ignores them will display confident nonsense for some Sony and Olympus
  files. Consider making the function return `RIA_ERR_NO_DATA` rather than
  `RIA_OK` when `locus == OFF`, forcing the caller to opt in to an unreliable
  number — heavier-handed, but harder to misuse. **Decide before 0.2.0
  ships**; changing it afterwards is a breaking change.
- **The linearity fix changes output for existing callers.** Only
  `raw_viewer` consumes this library and it does not use adjustments, so the
  window to do this cleanly is now.
- **Knot positions and the ±3 EV range interact badly** (§5). Calibrate
  before locking the defaults into the ABI.
- **No independent validator is installed.** `exiftool` for Canon's own
  `ColorTemperature` tag, and optionally `lcms2` in the test build, should
  both be in place before feature 1 is called done. Testing a colour
  temperature implementation only against itself proves nothing.
