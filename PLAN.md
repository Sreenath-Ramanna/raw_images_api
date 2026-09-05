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
>
> **Revision 3.** Measuring the sensor data and LibRaw's highlight modes moved
> the float sample type out of Phase A. Bit depth is precision, not range: a
> linear decode already leaves 2–3 stops of headroom, and the intermediates
> that exceed it live in float registers on the fused path rather than in a
> buffer. Phase A drops from 2 sittings to 1.5; float becomes unscheduled
> Phase G. Also found: the Canon's decode clipping is *manufactured by white
> balance*, not inherited from the sensor, and `highlight_mode` > 0 rescales
> the whole image by up to two thirds of a stop.

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
correct answer is 184 — the naive result is 0.93 EV too bright, and the
error grows with input value until everything above encoded 192 is pinned at
white.
`tests/test_ria.c` asserts the wrong behaviour and pins the defect in place.
It is removed, not fixed — see approach.md §10.

---

## 2. Phases

**The saturation anchor and the colourspace matrices are not a phase.** They
add only what the C side alone can supply — the highlight rescale
(`ria_image.saturation_level`) and LibRaw's `out_rgb[]` table
(`ria_colorspace_from_srgb`) — and none of B–G is started by them. Phase C is
the one that would *also* append to `ria_decode_options`, and the two must not
be in flight at once: morphosis `calloc`s that struct at the Dart size while
`ria_decode_options_scene_linear` `memset`s at the C size, so an unmirrored
append is a heap overflow rather than a benign mismatch.

### Phase A — scene-referred foundation ✅ done

**Blocks everything. Nothing below is correct without it.**

Landed. `RIA_DISPLAY_CLIP` with default parameters reproduces a LibRaw decode
to a mean difference of **0.19 code values, worst case 1**, which is the check
that the reimplementation of dcraw's `gamma_curve` is faithful. End to end on a
test frame: the scene path at `+1.7 EV` reaches the same mean brightness as the
default decode — confirming the auto-brightness measurement — and `+2.5 EV`
with a shoulder reaches it while clipping **0.00 %** against the default's
0.74 %.

Two deviations from what was planned, both explained below: the fused render
entry point moved to Phase E, and the preset uses `highlight_mode 0` rather
than 2.

- Linear decode preset: `gamma 1.0`, `output_bits 16`, `no_auto_bright 1`,
  `highlight_mode 0`. Verified — the median lands at −3.11 EV and −3.09 EV on
  two different cameras, against −0.71 EV for the display default, and 1.0
  means sensor saturation on both. The preset still uses `highlight_mode 0`; a
  caller who sets a higher mode now gets the applied scale reported on
  `ria_image.saturation_level` rather than an unexplained brightness shift.
- `ria_image` gains `transfer`, `transfer_gamma`, `transfer_slope`,
  `colorspace` (all appended; the struct stays ABI-compatible). A decoded image
  now knows what domain it is in.
- Display transform (`ria_apply_display_transform`), with `RIA_DISPLAY_CLIP`
  reproducing today's behaviour exactly so nothing regresses, and
  `RIA_DISPLAY_SHOULDER` for the extended-Reinhard rolloff.
- ~~The fused render path (`ria_render_scene_to_display`)~~ — **moved to
  Phase E.** There is nothing to fuse until the tone engine exists;
  `ria_apply_display_transform` is already a single LUT-driven pass with float
  intermediates, so the correctness requirement is met for now. The requirement
  itself stands and lands with the tone engine: a materialised intermediate
  between the two would clamp everything above 1.0, which is exactly what the
  shoulder exists to compress.
- Remove `ria_adjustments.exposure_ev`; rewrite the tests that asserted it.
  Done — including the assertion that pinned the defect. `ria_suggest_adjustments`
  now proposes a `gamma` instead, which is the display-domain way to move the
  midtones without dragging the endpoints.

**No float sample type.** Measurement moved this out of Phase A — see the
re-scoping note below.

**Effort: 1.5 sittings estimated; roughly that in practice.** Most of the
unplanned time went on two things worth recording: reimplementing dcraw's
gamma curve faithfully rather than approximating it, and discovering the
`highlight_mode` anchor problem by rendering the result.

#### Why float left Phase A

Bit depth is **precision, not range**, and the two halves of "adjust without
clipping" have different answers:

- *Shadows* — depth solves it. 16-bit linear gives 128 code values in the
  −8…−7 EV stop, against 20–30 per stop anywhere in 8-bit gamma. A +3 EV
  shadow lift posterises in 8-bit and does not in 16-bit linear.
- *Highlights* — depth does nothing. Both depths normalise to [0, max] and
  clip at the same *value*. Demonstrated in approach.md §0: two **16-bit**
  decodes, one clipping 1.01 % and the other 0.00 %.

What buys top-end headroom instead, all measured:

| | effect |
|---|---|
| linear + `no_auto_bright` | clipped 1.41 % → 0.00 % (Nikon), 2.25 % → 0.13 % (Canon) |
| the resulting headroom | 2–3 stops above the 95th percentile |
| float registers in the fused path | unbounded, and free |

Since the linear decode leaves 2–3 stops spare and the intermediates that
exceed 1.0 live in registers rather than a buffer, **16-bit linear covers the
realistic adjustment envelope**. A `+3 EV` *shadow* lift applies near-zero gain
at the top end by construction — that is what the zone weights are for. The
case that genuinely overflows is a large global positive exposure on an
already-bright frame, which is real but not the common path.

Float therefore becomes Phase G, wanted when a caller needs to hold a
scene-referred buffer across many operations without thinking about clamping.
Phase A only has to keep it *possible*, which it does: `sample_type` is an
appended field and every operation already switches on `bits`.

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

### Phase G — float sample type

`RIA_FMT_RGB32F` / `RIA_FMT_RGBA32F`, `ria_sample_type` appended to
`ria_image`, `ria_image_convert` in both directions, and float paths in the
scene-referred operations only. Existing display-domain operations return
`RIA_ERR_UNSUPPORTED` for float with a documented reason, rather than growing a
third branch in every loop.

**Unscheduled.** Build it when a caller wants to hold an editable
scene-referred buffer across many operations — not before. Costs 400 MB for a
33 MP RGB frame against 200 MB for 16-bit.

**Effort: 1.5 sittings when it happens.**

---

## 3. Sequencing and releases

| Phase | Depends on | Effort |
|---|---|---|
| **A** scene-referred foundation | — | 1.5 |
| **B** read colour temperature | A (not strictly, but ships together) | 2 |
| **C** decode at a target temperature | B | 0.5 |
| **D** zone histogram | A | 1.5 |
| **E** EV tone engine | A, D | 2 |
| **F** chromatic adaptation | A, B | 1.5 |
| **G** float sample type | A | *unscheduled* |

*(Sittings, not days.)*

**0.2.0 = A + B + C** — "read and set colour temperature", plus the
scene-referred foundation underneath it. A coherent release even though most
of the work is invisible.

**0.3.0 = D + E** — "zone and EV tone control". F folds into whichever is
convenient; it changes no API that D or E depend on.

A is genuinely non-negotiable as first. B before C because C needs the
CCT→multiplier conversion B builds. D before E because E consumes D's weight
functions. G is deliberately outside the sequence — see the re-scoping note
under Phase A.

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
- **Float support across the existing display-domain operations.** Phase G
  adds it to the scene-referred operations only. Spreading it through
  `ria_image.c`, `ria_adjust.c` and `ria_filter.c` is a large mechanical change
  that should wait for a caller who needs it.

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
- **The fused path is load-bearing and easy to get wrong.** Writing the
  scene-referred result to a 16-bit buffer before the display transform clamps
  everything above 1.0, and the highlight shoulder then compresses nothing.
  The result looks plausible — highlights merely flat — so it can ship
  unnoticed. approach.md §11 specifies the test that catches it: compare
  `RIA_DISPLAY_SHOULDER` against `RIA_DISPLAY_CLIP` on a `+2 EV` render, and
  fail if they agree.
- ~~**`highlight_mode` is coupled to exposure.**~~ **Resolved.** The rescale is
  `min(pre_mul[c])` after `dcraw_process` — 0.5401 on the Nikon frame and
  0.5206 on the Canon, against paired-decode median ratios of 0.54002 and
  0.520502. The decode reports it on `ria_image.saturation_level` and leaves
  the pixels alone, so a UI toggle recovers highlight detail without moving the
  EV scale.
- **Memory.** 16-bit linear is 200 MB for a 33 MP frame against 130 MB for
  today's 8-bit RGBA — a real increase, but bounded. Phase G's float buffers
  would be 400 MB, which is one reason they are unscheduled.
- **CCT is not always meaningful and the API must not pretend otherwise.** See
  the open decision above.
- **No independent validator is installed.** `exiftool` reads Canon's own
  `ColorTemperature` MakerNote tag — the strongest available ground truth for
  feature 1. `lcms2` (already present, 2.16) gives an independent xy → CCT for
  the test binary. Both should be in place before Phase B is called done.
  Testing a colour temperature implementation only against itself proves
  nothing.
