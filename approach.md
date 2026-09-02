# Approach — colour temperature and EV-based tone control

How the five requested features should be implemented, and why the obvious
implementation of most of them is wrong.

This document is the *how*. [PLAN.md](PLAN.md) is the *what and when* —
sequencing, effort, prior art, open decisions.

---

## 0. The correction

The first revision of the plan proposed doing EV work on the decoded image by
undoing the output gamma, adjusting, and re-encoding. That is a workaround, not
a fix. It gets the arithmetic right and the data wrong: by the time
`ria_raw_decode` returns, LibRaw has already applied a tone curve and clipped
the highlights, and no amount of correct arithmetic afterwards recovers them.

It also conflated exposure with brightness, and left unspecified whether
contrast acts per channel or on luminance — which decides whether it shifts
hue.

The corrected approach: **do tone work on scene-referred linear data, and make
the display transform an explicit, separate stage.** Everything below follows
from that.

### The measurement that settles it

Decoding the same frame four ways, and reporting luminance percentiles as EV
relative to sensor saturation (half-size decodes, `ria_tool`-adjacent probe):

| decode | median | p95 | clipped |
|---|---|---|---|
| default: gamma 2.222, 8-bit, auto-bright | −0.71 EV | −0.14 EV | **1.41 %** |
| gamma 2.222, 16-bit, auto-bright | −0.71 EV | −0.14 EV | 1.01 % |
| **linear, 16-bit, no auto-bright** | **−3.11 EV** | **−2.00 EV** | **0.00 %** |
| linear, 16-bit, auto-bright | −1.39 EV | −0.28 EV | 1.01 % |

*(Nikon Z 6_2 NEF; the Canon EOS R7 CR3 behaves the same — median −3.09 EV and
0.13 % clipped in the linear case, against 2.25 % in the default.)*

Two things fall out of this table:

1. **The default decode clips 1–2 % of the frame** before any adjustment is
   applied. Those pixels are gone. A "+1 EV highlights" control operating on
   that output is polishing data that no longer exists.
2. **`no_auto_bright = 0` is applying an uncontrolled +1.7 EV**, and the
   amount is scene-dependent — it comes from LibRaw's histogram analysis. Any
   control calibrated in stops sits on top of an unknown offset, so "+1 EV"
   does not mean the same thing on two different frames.

The linear decode has the median at −3.1 EV with two stops of headroom above
the 95th percentile and essentially nothing clipped. That is the data these
features need.

### Where the clipping actually comes from

A second measurement, going back to the sensor data itself and then varying
LibRaw's highlight handling:

| | Nikon Z 6_2 | Canon EOS R7 |
|---|---|---|
| sensor black / white level | 1008 / 16383 | 2047 / 16383 |
| brightest photosite, vs white | **−1.55 EV** | 0.00 EV |
| photosites at or above white | ~0 % | < 0.0005 % |
| linear decode clipped, `highlight_mode 0` | 0.000 % | **0.126 %** |
| linear decode clipped, `highlight_mode 2` | 0.000 % | **0.000 %** |

Two things follow, and both matter more than bit depth does.

**Clipping in the Canon decode is created by white balance, not inherited from
the sensor.** Essentially no photosites are saturated, yet the decode clips
0.126 % of pixels. The as-shot multipliers scale red by ~1.92, so a red
photosite at 0.6 of saturation lands above 1.0 after balancing. This is the
concrete argument for §5's Mode A: **white balance decides what clips**, so it
has to be applied where the decision can still be made well — in camera space,
before demosaic, where `highlight_mode` can reconstruct from the channels that
did not clip.

**The Nikon frame has 1.55 stops of entirely unused sensor range** — it is
underexposed at capture. For that file, `+1.5 EV` is free in any format.

### A trap in `highlight_mode`

Modes above 0 **rescale the whole image**, not only the highlights:

| mode | Canon max output | clipped |
|---|---|---|
| 0 clip | 1.0000 | 0.126 % |
| 1 unclip | 0.6434 | 0.001 % |
| 2 blend | 0.7789 | 0.000 % |
| 3 rebuild | 0.9997 | 0.001 % |

Changing the mode therefore changes the brightness of the result. Measured on
the linear preset, mode 2 moves the **median** by −0.88 EV on the Nikon frame
and −0.94 EV on the Canon, and leaves the maximum at 0.154 on one and 0.779 on
the other.

That second number is the problem. The scene-referred domain is only useful if
1.0 means something fixed, and under `highlight_mode 0` it does — sensor
saturation, on every file, with the median landing at −3.11 EV and −3.09 EV on
two different cameras. Under mode 2 the anchor moves by a file-dependent
amount, and since the zone system (§6) measures EV relative to white, a white
that moves per file makes "shadows" mean something different in every image.

**So the linear preset uses `highlight_mode 0`**, accepting 0.126 % clipping on
the Canon frame (none on the Nikon) in exchange for a stable EV scale. A caller
who would rather have that highlight detail can set the mode themselves, and
must then expect a brightness shift of up to a stop and an EV scale that is no
longer comparable across files.

A UI exposing this as a "highlight recovery" toggle will appear to have a
hidden exposure slider attached. Normalise against a reference decode, or
document the coupling loudly.

---

## 1. Two domains

| | scene-referred | display-referred |
|---|---|---|
| values are | proportional to photons | code values for a display |
| range | [0, sensor saturation], no rolloff yet | [0, 1], rolled off and clipped |
| ratios mean | light ratios — **EV is meaningful** | nothing photometric |
| `×2` means | one more stop of exposure | a brightness change of no fixed size |
| right for | exposure, white balance, zones, contrast | black/white point, output gamma, 8-bit delivery |

The rule for placing an operation: **if it is specified in EV, it belongs in
the scene-referred domain.** If it is specified as a position on a 0–255
slider, it belongs in the display domain.

By that rule, of the five requested features, four are scene-referred
(colour temperature, temperature adjustment, zone EV, exposure/contrast) and
one spans both (histograms — see §6).

---

## 2. The pipeline

```
  RAW file
     │
     │  ria_raw_decode, linear preset:
     │    gamma 1.0, output_bits 16, no_auto_bright 1,
     │    highlight_mode 0, WB as shot or as requested
     ▼
  ┌──────────────────────────────────────────────┐
  │  SCENE-REFERRED  (linear, 1.0 = saturation)  │
  │                                              │
  │   white balance / temperature   (§5)         │
  │   exposure                      (§8)         │
  │   zone EV adjustment            (§7)         │
  │   contrast                      (§8)         │
  │                                              │
  │   analysis: zone histogram      (§6)         │
  └──────────────────────────────────────────────┘
     │
     │  ria_apply_display_transform  (§9)
     │    grey point, highlight shoulder, output curve
     ▼
  ┌──────────────────────────────────────────────┐
  │  DISPLAY-REFERRED  (8 or 16-bit encoded)     │
  │                                              │
  │   black / white point, saturation, vibrance  │
  │   unsharp mask, resize                       │
  │   analysis: 256-bin histogram   (§6)         │
  └──────────────────────────────────────────────┘
     │
     ▼  delivery
```

Everything currently in `ria_adjustments` sits in the lower box and stays
there. The upper box is new.

**The stages must not be materialised separately** on the common path. Writing
the scene-referred result to a 16-bit buffer before the display transform
clamps anything the tone engine pushed above 1.0, which is precisely the data
the highlight shoulder exists to roll off. Fusing the two is a correctness
requirement, not an optimisation — see §3. The boxes are a statement about
*order and domain*, not about buffer count.

---

## 3. Working format

### Bit depth is precision, not range

These are separate properties and only one of them is about clipping.

**Depth is precision.** Linear encoding spends code values unevenly: in 16-bit
linear the number of distinct codes in the stop below *n* EV is
`65535 × 2^−(n+1)`:

| stop | codes available |
|---|---|
| −1 … 0 EV | 32768 |
| −6 … −5 EV | 512 |
| −8 … −7 EV | 128 |
| −10 … −9 EV | 32 |

8-bit gamma-encoded data gives roughly 20–30 codes per stop everywhere. So
**16-bit linear beats 8-bit gamma at every point down to about −10 EV**, and
the zone range this design cares about is −8…0 EV. Lifting shadows by 3 EV in
8-bit posterises; in 16-bit linear it does not. This is the half of the
question where more bits genuinely buy something.

**Range is not affected by depth at all.** Both depths normalise to
[0, max]; a value above white clips at 255 exactly as it clips at 65535. The
clip *point* is identical, the ruler below it is finer. The measurement in §0
demonstrates it directly: two **16-bit** decodes, one clipping 1.01 % and the
other 0.00 %. Same depth, and the difference is entirely where the data sat
relative to the ceiling.

*(A minor exception, visible in §0: the 8-bit gamma decode clipped 1.41 %
against 16-bit's 1.01 %. That 0.4 point gap is quantisation — in 8-bit more
near-white values round up to indistinguishable-from-white. Real, but a
rounding effect, not headroom.)*

### How much headroom does the linear decode actually leave?

Measured, after `gamma 1.0` + `no_auto_bright` + `highlight_mode 0`:

| | Nikon | Canon |
|---|---|---|
| median | −3.11 EV | −3.09 EV |
| p95 | −2.00 EV | −1.60 EV |
| p99.9 | −1.88 EV | −1.37 EV |
| brightest pixel | 0.284 (−1.81 EV) | 1.000 (0.00 EV) |

So the linear decode leaves **2–3 stops of unused range above the 95th
percentile**. Within that, `+2 EV` globally is essentially free, and the
adjustments these features actually make are smaller than the worst case
suggests: a `+3 EV` *shadow* lift applies near-zero gain at the top end,
because that is exactly what the zone weights are for (§7). The case that
genuinely exceeds the container is a large global positive exposure on an
already bright frame — real, but not the common path.

### Recommendation: 16-bit linear now, float when someone needs it

**Phase A ships 16-bit linear and no float sample type.** The reasoning:

1. Shadow precision — the half of the problem depth solves — is handled.
2. The linear decode leaves 2–3 stops of headroom, so the realistic
   adjustment envelope fits.
3. **The intermediate values that exceed 1.0 never need to live in a buffer.**
   See the fused path below. Headroom is needed *during the computation*, and
   a float register costs nothing.

A float sample type is still worth having eventually, for the caller who wants
to hold a scene-referred buffer across many operations and not think about
clamping:

```c
RIA_FMT_RGB32F  = 6,
RIA_FMT_RGBA32F = 7,
typedef enum { RIA_SAMPLE_UINT = 0, RIA_SAMPLE_FLOAT = 1 } ria_sample_type;
/* appended to ria_image */
ria_sample_type sample_type;
```

But it costs **400 MB for a 33 MP RGB frame** against 200 MB for 16-bit, and
adding a third sample type to every loop in `ria_image.c`, `ria_adjust.c` and
`ria_filter.c` is a large, mechanical, bug-prone change. Defer it until a
caller has a use for it, then add it narrowly: `ria_image_convert` in both
directions, the scene-referred operations, and nothing else — existing
display-domain operations return `RIA_ERR_UNSUPPORTED` for float with a
documented reason.

The one thing to get right *now* is that the appended-field plan above stays
possible. It does: `sample_type` is appended, and every operation already
switches on `bits`.

### The fused path — where the headroom actually lives

This is what makes the deferral safe rather than merely convenient.

The tone engine (§8) is a one-dimensional function of luminance and the
display transform (§9) is a one-dimensional function of value, so the whole
chain collapses into a single pass:

```
16-bit linear in
   → compute Y, look up gain            ─┐
   → scale RGB                           │  all in float registers,
   → display transform: shoulder + curve │  never written to memory
   → quantise                           ─┘
→ 8-bit display out
```

A pixel that the tone engine pushes to 4.0 is held in a float register, rolled
off by the shoulder, and lands inside [0,1] before anything is stored. **It is
never clamped, because it is never written to a 16-bit buffer.** The headroom
exists exactly where it is needed and costs one register.

This is also the only way the shoulder in §9 does any work: a shoulder applied
to data that has already clamped at 1.0 has nothing to compress. Fusing is
therefore not an optimisation here, it is a correctness requirement — a
materialised 16-bit scene buffer between the tone engine and the display
transform would silently defeat the highlight rolloff.

Offer it as `ria_render_scene_to_display()` and make it the documented default
path. Two lookups and three multiplies per pixel, no intermediate allocation.

---

## 4. Feature 1 — the recorded colour temperature

Unaffected by the correction above: this is metadata reading, not pixel work.
The design from the previous revision stands, and is summarised here for
completeness.

`ria_raw_white_balance()` returns CCT, tint, Duv, the white point in CIE xy,
which method produced it, and a reliability flag. Two paths:

- **Camera table** — when `color.WBCT_Coeffs` is populated (the EOS R7 carries
  15 rows from 3200 K to 10900 K; the Z 6_2 carries none), reparameterise it in
  **mired** rather than Kelvin and search for the entry closest to the
  normalised as-shot multipliers. Mired because the tables are near-uniform in
  it and wildly non-uniform in Kelvin — interpolating in Kelvin biases every
  result warm.
- **Colorimetric** — always available. Camera neutral `(1/mul_r, 1/mul_g,
  1/mul_b)` → `cam_xyz⁻¹` → XYZ → xy → CCT by Robertson's method, with Duv
  falling out of the same walk.

The residual perpendicular to the locus **is the tint**, and it must be
reported rather than discarded: the R7 test file's red ratio implies above
6000 K while its blue ratio implies below 5600 K, because the white point sits
off the Planckian locus. A single Kelvin number cannot describe it.

`reliable` and `locus` are load-bearing. Some Sony and Olympus bodies produce
coefficients for which no CCT is meaningful at all.

Full detail — struct definitions, the Robertson walk, the daylight-versus-black-body
split, the validation strategy — is in PLAN.md §2, which the correction does
not change.

---

## 5. Feature 2 — adjusting to a different colour temperature

White balance is a **per-channel scale on linear data**. That is all it is.
Everything that makes it complicated is about *where in the pipeline* the
scaling happens.

### Mode A — in the decode (exact)

```c
/* appended to ria_decode_options */
float target_cct;    /* Kelvin; 0 = as shot */
float target_tint;   /* 1.0 = neutral       */
```

Convert (CCT, tint) to multipliers by inverting §4, write them to
`lr->params.user_mul[]`, clear `use_camera_wb`, decode. The multipliers are
applied to **camera-space linear sensor data before demosaic**, which is the
only place they can be applied without consequence.

Why it matters that this is before demosaic and before clipping: the three
channels saturate at different scene luminances, and **white balance decides
which one clips first**. Doing it early means LibRaw's highlight handling
(`highlight_mode`) sees the corrected data and can reconstruct from the
unclipped channels. Doing it late means the clip pattern is already baked in.

The measurement in §0 makes this concrete rather than theoretical. The Canon
frame has essentially **no saturated photosites** — its brightest is exactly at
the white level and fewer than 0.0005 % reach it — yet the decode clips
0.126 % of output pixels at `highlight_mode 0`. That clipping is manufactured
by the white balance: red is scaled by ~1.92, so a red photosite at 0.6 of
saturation lands above 1.0 after balancing. Change the target temperature and
you change which pixels clip and in which channel. That is not something a
post-hoc adaptation can undo.

Cost: a re-decode, 1.5–3 s. The handle already re-reads the file when a decode
has consumed it, so this is one call.

### Mode B — on scene-referred linear (fast, and now nearly exact)

The previous revision described this as a rough approximation. On
**display-encoded** data it is. On **scene-referred linear** data it is very
nearly exact, and this is the single biggest practical gain from the
correction.

After demosaic the pixels are in the output colour space, not camera space, so
a per-channel scale is no longer the right operation — a proper chromatic
adaptation is:

1. Working RGB → XYZ, using the primaries of `image->colorspace`.
2. **Bradford adaptation** from the source white point to the target: XYZ →
   LMS via the Bradford matrix, per-channel scale by the white-point ratio,
   back to XYZ.
3. XYZ → working RGB.

Steps 1–3 collapse into **one 3×3 matrix**, computed once, so the per-pixel
cost is nine multiplies. On linear data with headroom there is no encoding
loss and no clipping introduced by the operation itself.

```c
ria_status ria_white_balance_matrix(float from_cct, float from_tint,
                                    float to_cct, float to_tint,
                                    ria_colorspace, float matrix[9]);
ria_status ria_apply_color_matrix(ria_image*, const float matrix[9]);
```

`ria_apply_color_matrix` is worth exposing on its own — an arbitrary 3×3 in
linear light is also how you do colour space conversion, a cast fix, or a
channel mixer. One primitive, several features.

**What Mode B still cannot do:** recover highlights that clipped in camera
space during the decode. If the decode clipped the red channel and you then
cool the image, the clipped region has no red detail to cool. For nudges of a
few hundred Kelvin this is invisible; for a 3000 K correction it is not. The
documented pattern: **adapt for the preview, re-decode for the export.**

---

## 6. Feature 3 — histograms and zones

### Two histograms, because there are two questions

The existing `ria_compute_histogram` — 256 bins of display-encoded value for
R, G, B and luma — is correct for *drawing a histogram widget*, which is a
statement about the delivered image. It stays exactly as it is. The only
addition is a mode flag so a caller can skip the per-channel work:

```c
typedef enum {
    RIA_HIST_LUMA = 1 << 0,
    RIA_HIST_RGB  = 1 << 1,
    RIA_HIST_ALL  = RIA_HIST_LUMA | RIA_HIST_RGB,
} ria_histogram_mode;

ria_status ria_compute_histogram_ex(const ria_image*, ria_histogram_mode,
                                    ria_histogram* out);
```

Per-channel is already implemented and already costs nothing worth deferring —
the staging suggested in the original request is unnecessary. Luma-only saves
about 40 % of the pass, which is the only reason the flag exists.

For *exposure* work a different structure is needed, computed on
**scene-referred linear** data and binned in EV:

```c
#define RIA_ZONE_BINS 48                /* −10 … +2 EV in 0.25 EV steps */

typedef struct {
    uint32_t bins[RIA_ZONE_BINS];       /* by log2 luminance vs saturation */
    float    ev_min, ev_max;
    uint64_t pixels;
    double   mean_ev, median_ev;
    double   below_range, above_range;
} ria_zone_histogram;

ria_status ria_compute_zone_histogram(const ria_image*, ria_zone_histogram*);
```

It must reject display-encoded input — `RIA_ERR_INVALID` if
`image->transfer != RIA_TRANSFER_LINEAR` — because an EV axis computed from
gamma-encoded values is meaningless, and silently producing plausible numbers
is worse than refusing. The measurement in §0 shows why: the same frame reads
median −0.71 EV encoded and −3.11 EV linear.

Range −10…+2 EV: below −10 the data is noise, and the +2 accommodates values
pushed above saturation by a positive exposure adjustment.

### Black / shadow / highlight / white

The design question is how the boundaries behave, and getting it wrong here is
the most likely way for feature 4 to produce visibly broken images.

**Not hard thresholds.** If "shadow" is `EV < −4` and "highlight" is
`EV ≥ −4`, then lifting shadows by 2 EV puts a 2-stop cliff at exactly −4 EV.
Across a smooth gradient — sky, a wall, skin — that renders as a hard edge
where the scene has none. It is the classic posterisation failure of naive
zone tools.

**A partition of unity.** Four smooth weight functions over EV that sum to
exactly 1 everywhere. Then the analysis number ("how much of this image is
shadow") and the adjustment in feature 4 use *the same* definition and cannot
disagree about what a shadow is. That shared definition is the whole reason to
build it this way.

With knots `k₀ < k₁ < k₂ < k₃` in EV relative to saturation, and
`s(t) = 3t² − 2t³`:

```
w_black(e)     = 1                       e ≤ k₀
               = 1 − s((e−k₀)/(k₁−k₀))   k₀ < e < k₁
               = 0                       otherwise

w_shadow(e)    = s((e−k₀)/(k₁−k₀))       k₀ < e < k₁
               = 1 − s((e−k₁)/(k₂−k₁))   k₁ ≤ e < k₂
               = 0                       otherwise

w_highlight(e) = s((e−k₁)/(k₂−k₁))       k₁ ≤ e < k₂
               = 1 − s((e−k₂)/(k₃−k₂))   k₂ ≤ e < k₃
               = 0                       otherwise

w_white(e)     = s((e−k₂)/(k₃−k₂))       k₂ ≤ e < k₃
               = 1                       e ≥ k₃
               = 0                       otherwise
```

`s(t) + s(1−t) = 1` identically, so the four sum to 1 — provable, and a
one-line test. Smoothstep rather than a linear ramp because it is C¹: the
slope is continuous at the knots too, so a strong adjustment leaves no visible
crease.

```c
typedef struct {
    double black, shadow, highlight, white;   /* fractions, sum to 1 */
    double mean_ev, median_ev;
    double clipped_black, clipped_white;      /* true clipping, not zones */
    float  knots_ev[4];
} ria_zone_summary;

ria_status ria_zone_summary_from(const ria_zone_histogram*,
                                 const float knots_ev[4] /* NULL = default */,
                                 ria_zone_summary* out);
void       ria_zone_weights(float ev, const float knots_ev[4], float w[4]);
```

`ria_zone_weights` is public deliberately — it is the contract between
analysis and adjustment, and a caller drawing a zone overlay needs the same
numbers the engine uses.

**Knot defaults need calibrating against real frames.** The measured medians
above (−3.1 EV for both test files, p05 near −5.5 EV) suggest the interesting
range sits higher than the −8/−4/−1.5/0 starting point taken from where
Lightroom's regions roughly fall. Budget a session of reading zone summaries
across the test set and checking that a subjectively shadowy image reports a
high `shadow` fraction. See §7 for a second reason the spacing needs work.

---

## 7. Feature 4 — per-zone EV adjustment

```c
typedef struct {
    float black_ev, shadow_ev, highlight_ev, white_ev;  /* −3 … +3 */
} ria_zone_adjustments;
```

For each pixel, on scene-referred linear data:

```
e     = log2(Y / Y_sat)                    Y = Rec.709 luma of linear RGB
gain  = 2^( Σₖ wₖ(e) · zoneₖ_ev )
R,G,B ← R,G,B × gain
```

**All three channels by the same factor.** Scaling channels independently
would shift hue; a uniform scale changes luminance and leaves chromaticity
untouched, which is what "adjust the exposure of the shadows" means.

Because the gain depends only on luminance, it is a **one-dimensional
function** — tabulate it over quantised luminance (4096 entries is ample) and
the hot loop is: compute Y (3 multiplies), one lookup, three multiplies. No
`log2`, no `powf`, no branching. Estimated **120–160 ms for 33 MP**, in line
with the existing adjustment pass at 116 ms.

### The failure mode to design against

Adjacent zones pushed in opposite directions invert the tone curve. The slope
of the mapping is

```
d e_out / d e_in = 1 + Σₖ wₖ′(e) · zoneₖ_ev
```

and smoothstep has `|s′|max = 1.5 / span`. Between `k₂ = −1.5` and `k₃ = 0` the
span is 1.5 EV, so a 6 EV difference between `highlight_ev` and `white_ev`
gives `1 + 6 × 1.5/1.5 = −5`. **Strongly negative — local contrast inverts and
the image looks solarised.** That is reachable well inside the requested ±3 EV
range, so it is not theoretical.

Three responses, all needed:

1. `ria_tone_ev_min_slope()` reports the worst slope over the curve, evaluated
   across the 48 zone bins. A UI can warn or clamp on it.
2. A `soft_limit` flag scales **all four** zone deltas by a common factor until
   the minimum slope reaches a floor (0.05 is reasonable). A common factor
   rather than per-zone clamping preserves the shape the user asked for while
   making it realisable.
3. **Widen the knot spacing, or overlap the basis functions.** The tight
   `−1.5 → 0` gap is what makes the highlight/white pair fragile. Basis
   functions wider than the knot spacing — so three weights are non-zero at
   once rather than two — buy headroom at the cost of less independent
   controls. Settle this during the calibration session in §6.

### What this deliberately does not do

The gain depends on each pixel's *own* luminance. That preserves hue, produces
no halos, and is cheap — but it flattens local contrast at strong settings,
because a dark object against a bright background gets lifted exactly as much
as a genuinely shadowed region.

darktable's tone equalizer solves this with a **guided filter** over a blurred
luminance mask, so the adjustment follows regions rather than pixels. That is
the right v2 and it is a substantial piece of work — a guided filter is its own
implementation and its own performance problem. It is out of scope here, and
importantly **the v1 API does not need to change to accommodate it**: it is an
internal change to how the mask luminance is computed, not to the parameters.

---

## 8. Feature 5 — exposure, brightness, contrast

This is where the original plan was wrong, so it is worth being precise.

### Exposure

**Exposure is a multiplication in scene-referred linear light**, uniform across
channels:

```
R,G,B ← R,G,B × 2^exposure_ev
```

It is the only post-capture operation that faithfully mimics having exposed
the sensor differently. Chromaticity is untouched. Range ±3 EV as requested,
though nothing in the maths breaks outside it.

**On linear data, post-hoc exposure is exact** — there is no tone curve for it
to fight, so scaling the decoded linear buffer gives the same result as having
scaled before the (nonexistent) curve. This is a genuine simplification over
the previous revision, which proposed a Mode A / Mode B split for exposure by
analogy with white balance. That split is unnecessary: **exposure needs no
re-decode, provided the decode was linear and un-auto-brightened.**

Two bounds on "exact", both from §3. Downward, 16-bit linear quantisation
means `−3 EV` then `+3 EV` recovers to within a few code values rather than
bit-identically. Upward, the result must not be written to a 16-bit buffer
before the display transform sees it, or anything above 1.0 clamps — which is
why the fused path is a correctness requirement and not a speed trick.

The one thing a re-decode still buys is highlight *reconstruction* — LibRaw's
`highlight_mode` 1–3 recovering detail from channels that did not clip. That is
a decode option worth setting up front, not a reason to re-decode per
adjustment.

### Brightness

**Brightness is not exposure, and should not be a separate EV control.**

The word has three incompatible meanings in circulation:

1. a linear scale — which *is* exposure, and offering both gives two sliders
   that do the same thing;
2. an additive offset in display space — the GIMP-style control, which crushes
   blacks and is almost never what a photographer wants;
3. a midtone lift that leaves black and white anchored — Lightroom's old
   Brightness slider.

Only (3) is a distinct, useful operation, and it is **not expressible in EV**,
because it is not a constant ratio: it moves the midtones by some amount and
the endpoints by nothing. Quoting it in stops would be a lie about what it
does.

**Recommendation: do not add `brightness_ev`.** Provide meaning (3) where it
naturally belongs — as the **grey point of the display transform** (§9), which
is exactly a midtone placement with the endpoints anchored. A caller who wants
"brighter" gets either `exposure_ev` (scene-referred, honest stops) or
`display.grey_point` (a midtone lift), and the documentation says which does
what. Two controls, two meanings, no overlap.

If a `brightness_ev` control is wanted anyway for interface reasons, define it
as an alias of `exposure_ev` and say so in one line, rather than inventing a
third behaviour.

### Contrast

Contrast is a **slope change about a pivot**. In scene-referred linear that is
a power law; in log2 space it is a straight-line slope, which is the same
thing:

```
e_out = p + slope · (e_in − p)          e = log2(Y), p = log2(pivot)
Y_out = pivot · (Y_in / pivot)^slope    equivalently
```

with

```
slope = 2^(contrast_ev / 3)
```

so `contrast_ev = 0` → slope 1; `+3` → slope 2, **the number of stops between
any two tones doubles**; `−3` → slope 0.5, halves it. `c` and `−c` compose to
the identity, which a test can assert. An additive definition such as
`(3+c)/3` was considered and rejected: it collapses to a flat grey at `c = −3`.

Pivot defaults to **0.18 linear** (−2.47 EV), the photographic middle grey.
Configurable, because a scene whose median sits at −3.1 EV — as both test files
do — may want the pivot lower.

**Contrast acts on luminance, not per channel.** This is a real choice with
visible consequences:

- *Per channel* (`R^s, G^s, B^s`): raises apparent saturation along with
  contrast, which is what people expect "punchy" to look like — but it shifts
  hue for non-neutral colours, and produces the notorious blue-to-purple twist
  in skies.
- *On luminance* (compute Y, curve Y, scale RGB by the ratio): hue and
  saturation ratios are exactly preserved, but the result looks flat, because
  perceived colourfulness tracks luminance contrast.

**Take luminance-only**, for three reasons: it is predictable (hue never moves
unless asked), it composes correctly with the zone system which is also
luminance-driven, and the flatness is fixable with the `saturation` and
`vibrance` controls that already exist — making that explicit is better than
baking an unasked-for hue shift into a contrast slider. Document the flatness
and the remedy together, so the first person to notice it finds the answer
next to the problem.

### One engine

Exposure, contrast and the four zone controls are all functions from input EV
to output EV, applied as a luminance-preserving gain. Building them separately
would mean three passes and three rounding steps for one curve.

```c
typedef struct {
    float exposure_ev;        /* −3 … +3, linear-light scale             */
    float contrast_ev;        /* −3 … +3, slope = 2^(c/3)                */
    float contrast_pivot;     /* linear luminance, default 0.18          */

    float black_ev, shadow_ev, highlight_ev, white_ev;   /* −3 … +3      */
    float knots_ev[4];        /* all zero = library default              */
    int   soft_limit;         /* 1 = scale zones back to stay monotonic  */
} ria_tone_ev;

void       ria_tone_ev_defaults(ria_tone_ev*);
ria_status ria_apply_tone_ev(ria_image*, const ria_tone_ev*);
float      ria_tone_ev_min_slope(const ria_tone_ev*);
```

Evaluation order, on `e = log2(Y / Y_sat)`:

```
e₁ = e + exposure_ev + Σₖ wₖ(e) · zoneₖ_ev      exposure-like terms
e₂ = p + slope · (e₁ − p)                        contrast, last
gain = 2^(e₂ − e)
```

Exposure and zones first because they are both scene-referred scales;
contrast last because it is tonal shaping applied to the result. **Zone
weights are evaluated on the input EV `e`, not on `e₁`** — evaluating them on
the shifted value makes the equation implicit and needs per-pixel iteration for
no visible benefit. That deserves a comment in the source, or someone will
"fix" it.

---

## 9. The display transform

Features 4 and 5 are not meaningful without specifying this stage, because it
decides what happens to everything the adjustments pushed above saturation.

```c
typedef enum {
    RIA_DISPLAY_CLIP = 0,      /* gamma, hard clip at 1.0 — LibRaw's behaviour */
    RIA_DISPLAY_SHOULDER,      /* extended Reinhard: smooth highlight rolloff  */
} ria_display_mode;

typedef struct {
    ria_display_mode mode;
    float  grey_point;         /* linear value mapped to display middle grey */
    float  white_ev;           /* scene EV mapped to display white           */
    float  black_ev;           /* scene EV mapped to display black           */
    ria_transfer transfer;     /* output curve: sRGB, gamma, linear          */
    float  gamma, slope;
} ria_display_transform;

ria_status ria_apply_display_transform(const ria_image* scene,
                                       const ria_display_transform*,
                                       ria_pixel_format out_fmt,
                                       ria_image** out);
```

`RIA_DISPLAY_CLIP` reproduces today's behaviour exactly, so the default path is
unchanged and nothing regresses.

`RIA_DISPLAY_SHOULDER` uses extended Reinhard, `Y' = Y(1 + Y/W²)/(1 + Y)`,
where `W` is the scene luminance mapped to display white. It is monotonic,
smooth, has no magic constants, and is about four lines. It is what makes
`+2 EV` on the highlights degrade gracefully instead of turning into a white
patch. A full filmic curve with a toe and a parametrised shoulder is a
reasonable later addition; Reinhard is the right amount of machinery for v1.

**The shoulder only works on unclamped input**, so this stage has to be fused
with the tone engine rather than reading a materialised 16-bit scene buffer
(§3). A pixel the tone engine pushed to 4.0 must still be 4.0 when the
shoulder sees it; if it was stored and clamped to 1.0 first, the shoulder
compresses nothing and the highlights are flat white regardless of the mode
setting. This is the single easiest way to implement the whole design and get
no benefit from it.

`grey_point` is where the "brightness" control from §8 lives.

The whole transform is a 1-D function of luminance and folds into the same LUT
as the tone engine, so it is free in the fused path (§3).

---

## 10. Consequences for the existing API

| existing | fate |
|---|---|
| `ria_adjustments.exposure_ev` | **Removed in Phase A.** It multiplied display-encoded values, so `+1 EV` on mid-grey clipped to 255 where the correct answer is 184 — 0.93 EV too bright. It cannot be fixed in place; the correct operation needs scene-referred data. Replaced by `ria_tone_ev.exposure_ev` in Phase E, with `ria_display_transform.grey_point` covering it in the meantime. |
| `ria_adjustments.shadows`, `.highlights` | **Supersede** with the zone system, which is strictly more capable. Keep for one release with a doc note; remove at 0.3. |
| `ria_adjustments.contrast` | **Keep.** Display-space, 0.5 pivot, perceptually anchored — a different and legitimate control. Document the difference from `contrast_ev` explicitly. |
| `ria_adjustments.black_point`, `.white_point` | Keep. Display space, unrelated. |
| `ria_adjustments.saturation`, `.vibrance` | Keep. Display space, and now doubly useful as the remedy for luminance-only contrast (§8). |
| `ria_adjustments.wb_r/g/b` | **Deprecate** in favour of §5. A per-channel scale on display-encoded data is not a white balance. |
| `ria_compute_histogram` | Keep unchanged; `_ex` variant adds the mode flag. |
| `tests/test_ria.c` exposure assertions | **Rewrite.** They currently assert the defective behaviour (`+1 EV on 64 gives 128`) and would pin it in place. The replacement needs a comment saying why the expected value is what it is. |

Removing `exposure_ev` rather than silently changing its meaning is the right
call: a caller who upgrades gets a compile error and reads one line of
changelog, instead of images that quietly look different. The library is at
0.1.0 with a single consumer that does not use adjustments at all, so the
window for this is now.

---

## 11. Validation

Properties that must hold, all cheap to assert:

- **Round trips.** `contrast_ev = c` then `−c` is the identity.
  `cct → mul → cct` within 1 %. `−3 EV` then `+3 EV` is the identity *within
  quantisation* on 16-bit linear — assert a bound (a few code values), not
  exact equality, and tighten it to exact if a float buffer is ever added.
- **Partition of unity.** `Σ ria_zone_weights(e) == 1` for every `e` across
  the range, to float tolerance.
- **Monotonicity.** For a grid of zone settings across ±3 EV,
  `ria_tone_ev_min_slope() > 0` whenever `soft_limit` is on, and the tone
  curve applied to a ramp never decreases.
- **Hue preservation.** Applying exposure or contrast to a saturated patch
  leaves its chromaticity `(x, y)` unchanged to within rounding — this is the
  test that catches an accidental per-channel implementation.
- **Domain refusal.** `ria_compute_zone_histogram` on a display-encoded image
  returns `RIA_ERR_INVALID`, not a plausible wrong answer.
- **Linearity of the decode.** A synthetic check that the linear decode preset
  really is linear: decode the same file at two `bright` values and confirm the
  ratio is constant across the tonal range.
- **The shoulder actually shoulders.** Apply `+2 EV` through the fused path
  with `RIA_DISPLAY_SHOULDER` and confirm the highlights compress rather than
  flatten to white — specifically, that the top percentile of the output still
  has a spread of distinct values. This is the test that catches an
  accidentally materialised intermediate buffer, which would clamp the input
  to the shoulder and produce a plausible-looking but flat result. Compare
  against `RIA_DISPLAY_CLIP` on the same input: if the two agree, the fusion
  is broken.
- **`highlight_mode` coupling.** Assert that modes 1–3 rescale the output
  (measured: Canon max 1.0000 → 0.64/0.78/0.9997), so that if a LibRaw version
  changes this behaviour the renormalisation logic is known to need revisiting
  rather than silently shifting everyone's exposure.

Two external oracles, neither currently installed, both worth adding before
the colour temperature work is called done:

- **`exiftool`** — Canon writes its own `ColorTemperature` MakerNote tag.
  Comparing the camera-table path against it is the strongest ground truth
  available for feature 1.
- **`lcms2`** (MIT, already present on this machine at 2.16) — use
  `cmsTempFromWhitePoint` in the *test binary* as an independent implementation
  of the xy → CCT step. A test-only dependency costs nothing and catches real
  errors. It should not become a runtime dependency; the part needed is about
  60 lines.

Testing a colour temperature implementation only against itself proves
nothing.
