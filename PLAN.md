# Plan — colour temperature and EV-based tone control

Five features:

1. Read the colour temperature recorded in the RAW file.
2. Adjust decoded RGB to a different colour temperature.
3. Histograms in luminance and per-channel modes, plus a zone
   (black / shadow / highlight / white) representation.
4. Adjust pixels in a zone by an exposure value, −3 EV to +3 EV.
5. Adjust brightness and contrast by EV, −3 EV to +3 EV.

**[approach.md](approach.md) is the design.** This document is sequencing,
effort, prior art and open decisions.

> **Revision 2.** The first revision proposed doing EV work by undoing the
> output gamma on the decoded image, adjusting, and re-encoding. That is a
> workaround: it gets the arithmetic right and the data wrong, because the
> decode has already clipped the highlights. Measured, the default decode
> clips **1.4–2.3 %** of the frame and LibRaw's auto-brightness silently
> applies a scene-dependent **+1.7 EV** before any control is touched. The
> revised approach works on scene-referred linear data with the display
> transform as an explicit stage. Phase A changed completely as a result;
> §5 (feature 5) was rewritten; features 1–4 survived largely intact.

---

## 1. Does this already exist?

The algorithms all exist in open source. **None of it is reusable as a
library.** darktable and RawTherapee are the two mature implementations and
both are GPL *applications* — the code is entangled with their pipelines, GUIs
and image structures, so lifting it means either relicensing this library or
reimplementing from the same published maths. The latter is what everyone
does.

| # | Feature | Prior art | Reusable here? |
|---|---|---|---|
| 1 | CCT from a RAW file | darktable `temperature.c` / `colorin`; RawTherapee `ColorTemp::mul2temp`; Adobe DNG SDK `dng_temperature` | **No** — GPL applications. LibRaw gives the *inputs*, never the answer |
| 2 | Re-balance to a target CCT | RawTherapee `ColorTemp::temp2mul`; darktable white balance / color calibration | **Partly** — lcms2 (MIT) covers the CCT↔white-point half |
| 3 | Histogram | Everywhere | **Already built here** |
| 3b | Zone representation | Zone System; Lightroom's Blacks/Shadows/Highlights/Whites; darktable `tone equalizer` (9 zones spaced 1 EV) | Concept yes, code no |
| 4 | Per-zone EV adjustment | darktable `tone equalizer`; `gegl:shadows-highlights` | Concept yes, code no |
| 5 | Exposure in EV | darktable `exposure` iop, applied scene-referred before `filmic` | Trivial once the pipeline is right — which is the whole difficulty |

### Three findings that shaped the design

**LibRaw will not give you a Kelvin value.** It exposes the raw materials:
`color.cam_mul`, `color.cam_xyz`, and — *when the camera writes them* —
`color.WBCT_Coeffs[64][5]` (the camera's own Kelvin→multiplier curve) and
`color.WB_Coeffs[256][4]` (named presets). LibRaw's own position is that white
balance is not measured in Kelvin and that the reliable route is a table of CT
against coefficients, interpolated.

**Whether that table exists is vendor-dependent.** Probing the test images:

| Camera | `WBCT_Coeffs` rows | `WB_Coeffs` presets | Route |
|---|---|---|---|
| Canon EOS R7 (CR3) | **15** (3200–10900 K) | 9 | camera table **and** colorimetric |
| Nikon Z 6_2 (NEF) | **0** | 0 | colorimetric only |

So there are two paths, and the API must report which one produced a number.

**A single CCT cannot describe a white balance.** The R7 file's as-shot
multipliers normalise to R/G = 1.921, B/G = 1.568. Read against that camera's
own table the red ratio implies *above* 6000 K while the blue ratio implies
*below* 5600 K, because the white point sits off the Planckian locus. That
disagreement is the tint, and it must be reported rather than averaged away.
darktable labels a reported CCT "(daylight)", "(black body)" or "(invalid)"
and notes that some Olympus and Sony bodies are always invalid by that test.
Copy the honesty, not just the formula.

### What is already built

**Feature 3's per-channel half is done.** `ria_compute_histogram` already
returns 256 bins for R, G, B *and* luma at 38–51 ms on a 24–33 MP frame. The
staging suggested in the request — luminance now, per-channel later — is
unnecessary. What is missing is the *zone* view (approach.md §6).

**Feature 5's exposure half exists and is wrong.** `ria_adjustments.exposure_ev`
multiplies display-encoded values: `+1 EV` on mid-grey clips to 255 where the
correct answer is 174, an error of over a stop that grows with input value.
`tests/test_ria.c` asserts the wrong behaviour and pins the defect in place.
It is removed, not fixed — see approach.md §10.

---

## 2. Phases

### Phase A — scene-referred foundation

**Blocks everything. Nothing below is correct without it.**

- Linear decode preset: `gamma 1.0`, `output_bits 16`, `no_auto_bright 1`.
  Verified working — median lands at −3.1 EV with 0.00 % clipped, against
  −0.71 EV and 1.41 % for the default.
- `ria_image` gains `transfer`, `transfer_gamma`, `transfer_slope`,
  `colorspace`, `sample_type` (all appended; the struct stays ABI-compatible).
  A decoded image now knows what domain it is in.
- Float sample type (`RIA_FMT_RGB32F`, `RIA_FMT_RGBA32F`) with
  `ria_image_convert` support. **Not** propagated to every existing operation —
  see approach.md §3 for why that would be disproportionate.
- Display transform (`ria_apply_display_transform`), with `RIA_DISPLAY_CLIP`
  reproducing today's behaviour exactly so nothing regresses.
- Remove `ria_adjustments.exposure_ev`; rewrite the tests that asserted it.

**Effort: 2 sittings.** The float plumbing and the display transform are each
about a sitting; the test rework is small but must not be skipped.

### Phase B — feature 1, reading the colour temperature

`ria_raw_white_balance()`: CCT, tint, Duv, white point in xy, source, locus,
reliability. Camera-table path (mired-parameterised search) and colorimetric
path (`cam_xyz⁻¹` → xy → Robertson). Full design in approach.md §4.

**Effort: 2 sittings**, most of it validation rather than code. The Robertson
table and 3×3 inversion are about 150 lines; getting confidence in the answer
is the work.

### Phase C — feature 2 Mode A, decoding at a target temperature

`target_cct` / `target_tint` in `ria_decode_options`, converted to multipliers
by inverting Phase B and written to `lr->params.user_mul[]`. Small, because
Phase B did the hard part.

**Effort: 0.5 sittings.**

### Phase D — feature 3, zone histogram

`ria_compute_histogram_ex` mode flag; `ria_zone_histogram` (48 bins,
−10…+2 EV, scene-referred only); `ria_zone_summary`; `ria_zone_weights` as the
public contract between analysis and adjustment.

Includes a **calibration session**: read zone summaries across the test set and
check that a subjectively shadowy frame reports a high `shadow` fraction. The
default knots (−8, −4, −1.5, 0) are a starting point from where Lightroom's
regions roughly fall, and the measured medians at −3.1 EV suggest they want
moving.

**Effort: 1.5 sittings** including calibration.

### Phase E — features 4 and 5, the EV tone engine

One engine: exposure, contrast and four zone controls are all functions from
input EV to output EV, applied as a luminance-preserving gain (approach.md
§7–8). Tabulated over quantised luminance, one pass, estimated 120–160 ms on
33 MP.

The monotonicity work is the substance here: adjacent zones at opposite
extremes drive the curve slope to −5, which solarises the image, and that is
reachable inside the requested ±3 EV range. Needs `ria_tone_ev_min_slope()`,
the `soft_limit` fallback, and probably wider knot spacing.

**Effort: 2 sittings**, one of which is monotonicity.

### Phase F — feature 2 Mode B, chromatic adaptation

Bradford adaptation as a single 3×3 matrix on scene-referred linear, for
interactive temperature changes without a re-decode. `ria_apply_color_matrix`
exposed separately, since an arbitrary 3×3 in linear light is also colour space
conversion, cast correction and channel mixing.

Last because Mode A already delivers the feature correctly; this is the
interactive optimisation.

**Effort: 1.5 sittings.**

---

## 3. Sequencing and releases

| Phase | Depends on | Effort |
|---|---|---|
| **A** scene-referred foundation | — | 2 |
| **B** read colour temperature | A (not strictly, but ships together) | 2 |
| **C** decode at a target temperature | B | 0.5 |
| **D** zone histogram | A | 1.5 |
| **E** EV tone engine | A, D | 2 |
| **F** chromatic adaptation | A, B | 1.5 |

*(Sittings, not days.)*

**0.2.0 = A + B + C** — "read and set colour temperature", plus the
scene-referred foundation underneath it. A coherent release even though most
of the work is invisible.

**0.3.0 = D + E** — "zone and EV tone control". F folds into whichever is
convenient; it changes no API that D or E depend on.

A is genuinely non-negotiable as first. B before C because C needs the
CCT→multiplier conversion B builds. D before E because E consumes D's weight
functions.

---

## 4. Deferred

- **Spatially-aware zone adjustment.** The v1 engine maps each pixel by its own
  luminance: hue-preserving, halo-free, cheap, but it flattens local contrast
  at strong settings. darktable's tone equalizer solves this with a guided
  filter over a blurred luminance mask. Substantial work, and **the v1 API does
  not need to change to accommodate it** — it is an internal change to how the
  mask luminance is computed.
- **Filmic display transform** with a parametrised toe and shoulder. Extended
  Reinhard is the right amount of machinery for v1.
- **Per-channel zone adjustment** (split toning — warm highlights, cool
  shadows). Nearly free once the zone weights exist, but a different feature.
- **Tint as a first-class control** in Mode A. Feature 1 reports it; setting it
  needs the same table inversion with one more degree of freedom.
- **Float support across the existing display-domain operations.** Add it where
  a caller actually needs it, not pre-emptively.

---

## 5. Open decisions

Things to settle before the ABI sets, listed because getting them wrong is
expensive later.

**Should an unreliable CCT be an error?** `ria_raw_white_balance` could return
`RIA_OK` with `reliable = 0`, or `RIA_ERR_NO_DATA` when the illuminant is too
far off-locus for a Kelvin figure to mean anything. The second is
heavier-handed but much harder to misuse — a caller who ignores a flag displays
confident nonsense for some Sony and Olympus files. **Decide before 0.2.0
ships.**

**Knot positions and the ±3 EV range interact badly.** The tight −1.5 → 0 EV
gap makes the highlight/white pair the fragile one (approach.md §7). Widening
the knots, or making the basis functions overlap more than two at a time, buys
headroom at the cost of less independent controls. Settle during Phase D's
calibration, before the defaults are baked into the ABI.

**Does `ria_adjustments` survive?** After Phase E, `shadows` and `highlights`
are superseded, `wb_r/g/b` is deprecated, and `exposure_ev` is gone. What
remains — `black_point`, `white_point`, `contrast`, `saturation`, `vibrance`,
`gamma` — is a coherent display-space control set, but it is worth asking at
0.3 whether it should be renamed to say so.

---

## 6. Risks

- **The linearity fix changes what the library produces.** Only `raw_viewer`
  consumes it today and it does not use adjustments at all, so the window to do
  this cleanly is now. It closes as soon as there is a second consumer.
- **Memory.** A 33 MP RGB float buffer is 400 MB against 130 MB for today's
  8-bit RGBA. The fused path (approach.md §3) avoids materialising it for the
  common case, but the fused path has to actually get built, not just
  documented.
- **CCT is not always meaningful and the API must not pretend otherwise.** See
  the open decision above.
- **No independent validator is installed.** `exiftool` reads Canon's own
  `ColorTemperature` MakerNote tag — the strongest available ground truth for
  feature 1. `lcms2` (already present, 2.16) gives an independent xy → CCT for
  the test binary. Both should be in place before Phase B is called done.
  Testing a colour temperature implementation only against itself proves
  nothing.
