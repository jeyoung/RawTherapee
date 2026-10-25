# Stage pipeline specification

This record defines the canonical descriptor inventory and dependency graph for
the four engine pipelines: export, preview, crop and thumbnail. It is the
Phase A artefact for the stage runtime: the graph and its block boundaries are
specified and tested here, before any production pipeline is rewired.

The specification is data only. `rtengine/stage/stage_pipeline.cc` builds it and
`rtengine/stage/tests/stage_pipeline_test.cc` proves its shape. The thumbnail
path now executes this graph through `StageGraph::plan()` behind
`RT_STAGE_GRAPH=1`; the legacy sequence remains the default and is
byte-identical (see `STAGE_RUNTIME.md`). The other three paths are still
specification only.

## Scope

- Four paths: `StagePath::{Export, Preview, Crop, Thumbnail}`.
- Three domains: `Full`, `Roi`, `Reduced`.
- Two consumers: `Exact` (export) and `Interactive` (preview, crop,
  thumbnail).
- One canonical block catalogue, so ids, display names, reuse modes and
  parameter provenance are defined exactly once.
- Per path: one rooted, acyclic subgraph. Every node is reachable from a root
  and can reach a sink.

## Inventory sources

The block boundaries and the parameter provenance come from the current
implementation:

| Path      | Source                    | Entry points |
|-----------|---------------------------|--------------|
| Export    | `rtengine/simpleprocess.cc` | `processImage()` and the batch worker |
| Preview   | `rtengine/improccoordinator.cc` | `processImage()` / `worker()` |
| Crop      | `rtengine/dcrop.cc`       | `Crop::cropImage()` |
| Thumbnail | `rtengine/rtthumbnail.cc` | `loadQuickFromRaw()`, `quickProcessImage()`, `processImage()` |
| Invalidation | `rtengine/refreshmap.h`, `refreshmap.cc` | `M_*` block mask |

## Canonical block catalogue

Ids are stable. A block that a path does not run is absent from that path's
spec: `buildPipelineSpec()` returns the rooted subgraph, not the union.

| Id | Domain (export/preview/crop/thumbnail) | Consumer | Reuse | Role |
|----|----------------------------------------|----------|-------|------|
| `source.select` | Full/Full/Full/Full | path | recompute | select the frame to process |
| `source.embedded` | -/-/-/Reduced | interactive | recompute | select the embedded JPEG |
| `raw.preprocess` | Full/Full/Full/Full | path | reuse-output | raw black/white, hot/dead pixel, CA, flat field |
| `raw.demosaic` | Full/Full/Full/Full | path | reuse-output | demosaic and capture sharpening |
| `raw.base` | Full/Full/Roi/Reduced | path | reuse-output | `getImage()`, spot removal, denoise, film negative, Retinex, source-side gamut |
| `analysis` | Full/Full/Roi/- | path | reuse-output | histogram and auto-exposure analysis |
| `transform` | Full/Full/Roi/- | path | reuse-input | rotation, distortion, perspective, CA, vignetting, gradient, post-crop vignette, dehaze, Fattal |
| `local.adjustments` | Full/Full/Roi/Reduced | path | reuse-input | local lab / selective editing |
| `rgb.proc` | Full/Full/Roi/Reduced | path | reuse-input | RGB curves, tone curve, colour toning, B&W, channel mixer, HSV, film simulation |
| `lab.tools` | Full/Full/Roi/Reduced | path | reuse-input | shadows/highlights, Lab curves, local contrast, vibrance, EPD, Lab-grid colour toning |
| `lab.sharpening` | Full/-/Roi/- | path | reuse-input | impulse denoise, defringe, microcontrast, sharpening |
| `lab.wavelet` | Full/Full/Roi/Reduced | path | reuse-input | directional pyramid equaliser and wavelet |
| `working.trc` | Full/Full/Roi/- | path | reuse-input | soft light, tone equaliser and working-TRC conversion |
| `ciecam` | Full/Full/Roi/- | path | reuse-input | CIE CAM 02 |
| `gamut` | Full/Full/Roi/Reduced | path | reuse-input | working-space gamut compression |
| `output.convert` | Full/-/-/- | exact | reuse-output | Lab to output RGB and output profile conversion |
| `crop` | Full/-/-/- | exact | reuse-output | crop |
| `resize` | Full/-/-/- | exact | reuse-output | resize |
| `output.sharpen` | Full/-/-/- | exact | reuse-input | post-resize sharpening |
| `framing` | Full/-/-/- | exact | reuse-output | framing and frame drawing |
| `output.icc` | Full/-/-/- | exact | recompute | attach output ICC and metadata |
| `monitor.output` | -/Full/-/- | interactive | recompute | Lab to monitor RGB |
| `viewport.output` | -/-/Roi/- | interactive | recompute | Lab to monitor RGB for the viewport |
| `thumbnail.decode` | -/-/-/Reduced | interactive | reuse-output | decode the embedded JPEG |
| `thumbnail.scale` | -/-/-/Reduced | interactive | reuse-output | scale to thumbnail height |
| `thumbnail.transform` | -/-/-/Reduced | interactive | reuse-input | coarse rotate and flip |
| `thumbnail.whitebalance` | -/-/-/Reduced | interactive | reuse-input | apply white balance to the thumbnail |
| `thumbnail.rgb` | -/-/-/Reduced | interactive | reuse-input | thumbnail RGB processing |
| `thumbnail.lab` | -/-/-/Reduced | interactive | reuse-input | thumbnail Lab tools, working TRC, CIE CAM and gamut in one pass |
| `thumbnail.output` | -/-/-/Reduced | interactive | recompute | Lab to 8-bit thumbnail with gamma |

## Path specifications

### Export: 20 nodes, 19 edges

Root `source.select`, sink `output.icc`.

```text
source.select -> raw.preprocess -> raw.demosaic -> raw.base -> analysis
  -> transform -> local.adjustments -> rgb.proc -> lab.tools
  -> lab.sharpening -> lab.wavelet -> working.trc -> ciecam -> gamut
  -> crop -> resize -> output.sharpen -> output.convert -> framing
  -> output.icc
```

The last five nodes follow the implementation's data flow: the Lab image is
cropped, resized and post-sharpened before it is converted to the output
space, because `lab2rgbOut()` consumes the crop rectangle directly. The
catalogue ids, edges and reuse modes are unchanged from the inventory; only
the position of `output.convert` moved, from before `crop` to after
`output.sharpen`. Execution evidence is in `devnotes/perf/STAGE_RUNTIME.md`.

Code evidence, in order:

- preprocess `simpleprocess.cc:393`, demosaic `:402`, Retinex `:415-427`
- `getImage()` `:1001`, spot removal `:1043`, denoise `:1094`, film negative
  `:1124`, source gamut `:1146`
- `firstAnalysis()` `:1149`, dehaze `:1152`, Fattal `:1153`
- transform `:1156-1165`
- `Lab_Local()` `:1501`
- `rgbProc()` `:1657`, Lab-grid colour toning `:1730`
- shadows/highlights `:1733`, local contrast `:1737`, chromi/luminance curve
  `:1740`, EPD `:1746`, vibrance `:1750`, colour-correction regions `:1751`
- impulse denoise `:1757`, defringe `:1758`, microcontrast `:1762-1768`,
  sharpening `:1775`
- dirpyrequaliser `:1785`, wavelet `:1821` and `:1833`
- soft light `:1944`, complete local contrast `:1960`
- tone equaliser `:2012` and `:2048`, working TRC `:2020-2021`, saturation
  `:2025`, preservation `:2055`
- CIE CAM `:2123`, gamut `:2184`
- crop `:2210-2273`, Lab resize `:2275-2289`, post-resize sharpening
  `:2290-2298`
- `lab2rgbOut()` `:2316`, Nearest output resize `:2344-2354`, frame drawing
  `:2357`
- output ICC and metadata `:2387-2410`

The line numbers refer to the hand-written pipeline before the Phase B
decomposition; `devnotes/perf/STAGE_RUNTIME.md` records the method boundaries
that replace them.

### Preview: 14 nodes, 13 edges

Root `source.select`, sink `monitor.output`.

```text
source.select -> raw.preprocess -> raw.demosaic -> raw.base -> analysis
  -> transform -> local.adjustments -> rgb.proc -> lab.tools -> lab.wavelet
  -> working.trc -> ciecam -> gamut -> monitor.output
```

Code evidence, in order:

- preprocess `improccoordinator.cc:1659`, demosaic `:1732`, capture sharpening
  `:1750`
- `getImage()` `:2131`, spot removal `:2176`, denoise `:2219`, film negative
  `:2232`, source gamut `:2256`
- `firstAnalysis()` `:2263`, dehaze `:2292`, Fattal `:2293`
- transform `:2301-2312`
- `Lab_Local()` `:2906`
- `rgbProc()` `:3298`, Lab-grid colour toning `:3372`
- shadows/highlights `:3375`, local contrast `:3379`, chromi/luminance curve
  `:3382`, vibrance `:3383`, colour-correction regions `:3384`, EPD `:3388`
- dirpyrequaliser `:3394`, wavelet `:3427` and `:3440`
- soft light `:3612`, complete local contrast `:3627`
- tone equaliser `:3673`, working TRC `:3677-3678`, saturation `:3681`,
  preservation `:3730`
- CIE CAM `:3916`, gamut `:4063`
- `lab2monitorRgb()` `:4130`; the soft-proof `lab2rgb()` at `:4136` is folded
  into `monitor.output`

The full-frame preview does not run `lab.sharpening`: sharpening is only
applied by the crop viewport at 100% (`dcrop.cc:1435-1444`). It is therefore
absent from the preview spec and present in the crop spec.

### Preview runtime mapping

`RT_PREVIEW_GRAPH=1` drives the inventory through `StageGraph`; the legacy
sequence stays the default. Each block is one private
`ImProcCoordinator::stage*()` method, so both paths call the same
implementation in the same order:

| Node | Method |
|------|--------|
| `source.select` | `stageSourceSelect()` |
| `raw.preprocess` | `stageRawPreprocess()` |
| `raw.demosaic` | `stageRawDemosaic()` |
| `raw.base` | `stageRawBase()` |
| `transform` | `stageTransform()` |
| `local.adjustments` | `stageLocalAdjustments()` |
| `rgb.proc` | `stageRgbProc()` |
| `lab.tools` | `stageLabTools()` |
| `lab.wavelet` | `stageLabWavelet()` |
| `working.trc` | `stageWorkingTrc()` |
| `ciecam` | `stageCieCam()` |
| `gamut` | `stageGamut()` |
| `monitor.output` | `stageMonitorOutput()` |

`source.select` is the per-pass half of the source selection: the image
source itself is bound once by `ImProcCoordinator::assign()`, and the node
resolves the raw frame for the pass. `analysis` is folded into `raw.base`,
because `firstAnalysis()` is the last statement of the base-rebuild guard and
the spot pass that closes `raw.base` mutates `orig_prev` immediately
afterwards; a separate boundary would reorder the pass. The plan still
carries the node and tracks its dirty state, but no executor exists for it, so
the preview executes 13 of its 14 canonical nodes.

The nodes keep their own event-mask gates because a preview block decides how
much of its output a mask allows, not only whether it runs: `raw.base`
rebuilds the base for one class of bits, re-prepares retinex for another and
re-applies spots for a third. The plan decides whether a node is executed; the
node decides what the mask permits inside it. `monitor.output` executes at its
legacy site, after the crop render, so the monitor profile cache refresh it
follows still feeds the crop viewport.

Three statements that were unconditional in the hand-written pass stay in the
caller, never in a node: the `oprevi = orig_prev` working-pointer reset, the
local-adjustment damping floor, and the frame-count and image-type notices.
The Color/Coloropp `todo |= DEMOSAIC` augmentation also runs there so the plan
sees the mask the pass will use.

### Crop: 15 nodes, 14 edges

Root `source.select`, sink `viewport.output`. Only raw loading is full-frame;
every later block consumes a region of interest. The crop reuses the parent
preview's analysis data; the spec records `analysis` as a `Roi` node so the
core ids stay shared with the other paths.

```text
source.select -> raw.preprocess -> raw.demosaic -> raw.base -> analysis
  -> transform -> local.adjustments -> rgb.proc -> lab.tools
  -> lab.sharpening -> lab.wavelet -> working.trc -> ciecam -> gamut
  -> viewport.output
```

Code evidence:

- `getImage()` on the crop `dcrop.cc:238` and `:622`; spot removal `:629`;
  denoise `:670`; film negative `:690` and `:696`; source gamut `:707`
- dehaze `:798`, Fattal `:799`, transform `:840`
- `Lab_Local()` `:1146` and `:1239`
- `rgbProc()` `:1392`, Lab-grid colour toning `:1412`
- shadows/highlights `:1415`, local contrast `:1419`, chromi/luminance curve
  `:1421`, vibrance `:1422`, colour-correction regions `:1423`, EPD `:1427`
- impulse denoise/defringe `:1435-1436`, microcontrast/sharpening
  `:1443-1444`
- dirpyrequaliser `:1453`, wavelet `:1565` and `:1582`
- soft light `:1706`, complete local contrast `:1719`
- tone equaliser `:1772`, working TRC `:1779-1780`, saturation `:1783`,
  preservation `:1812`
- CIE CAM `:1855`, gamut `:1921`
- `lab2monitorRgb()` `:1942`

### Thumbnail: 12 nodes, 11 edges, two roots

Roots `source.embedded` (embedded JPEG) and `source.select` (raw fallback);
sink `thumbnail.output`. The branches join at `thumbnail.scale`, which is the
`thumbImg` member both `quickProcessImage()` and `processImage()` consume.

```text
source.embedded -> thumbnail.decode --+
                                      +-> thumbnail.scale
source.select -> raw.preprocess -> raw.demosaic -> raw.base --+

thumbnail.scale -> thumbnail.transform -> thumbnail.whitebalance
  -> thumbnail.rgb -> thumbnail.lab -> thumbnail.output
```

Code evidence. The blocks are the private `Thumbnail::stage*` methods in
`rtengine/rtthumbnail.cc`; each is the only implementation of that step,
shared by the legacy sequence and the graph-driven sequence.

- `source.embedded` `stageSourceEmbedded()`: open the raw container and
  select its embedded JPEG (`RawImage::getThumbnail()`). The container load is
  folded into this root because it exists only to reach the embedded image.
- `thumbnail.decode` `stageThumbnailDecode()`: nearest-neighbour resize to the
  requested size, coarse rotation and the Leaf `.mos`, Mamiya `.mef` and
  Phase One `.iiq` already-rotated exceptions.
- `source.select` `stageSourceSelect()`: open the raw container, resolve the
  frame (including the Hasselblad H6D-100cMS second frame) and record the
  sensor and crop geometry.
- `raw.preprocess` `stageRawPreprocess()`: black and white levels, scale
  multipliers, colour scaling and pre-interpolation.
- `raw.demosaic` `stageRawDemosaic()`: the reduced sampled demosaic and the
  Fuji geometry remap.
- `raw.base` `stageRawBase()`: finish the reduced raw image, including the
  histogram and auto white-balance analysis folded into this block.
- `thumbnail.scale` `stageScale()` (raw fallback) and `stageQuickScale()`
  (embedded branch): reduce `thumbImg` to the requested height. The embedded
  branch continues in 8-bit and the raw fallback in float, so the two entry
  points keep their own legacy arithmetic. `stageScale()` also runs the
  film-negative V1/V2 inversion before any geometry.
- `thumbnail.transform` `stageTransform()` / `stageQuickTransform()`: coarse
  rotate and flip, after `thumbnail.scale`.
- `thumbnail.whitebalance` `stageWhiteBalance()`: resolve the multipliers and
  apply them with the highlight-recovery magenta guard.
- `thumbnail.rgb` `stageRgb()`: colour-space conversion, first analysis,
  dehaze, Fattal tone mapping, geometry transform, local adjustments and
  `rgbProc()`.
- `thumbnail.lab` `stageLab()`: Lab tools, local contrast, working TRC,
  CIE CAM and gamut compression in one reduced-scale pass.
- `thumbnail.output` `stageOutput()`: Lab to 8-bit monitor or output RGB, the
  reported scale factor and the crop mask.

`thumbnail.lab` folds the Lab tools, working TRC, CIE CAM and gamut work that
the export and preview paths split into separate blocks, because the thumbnail
path runs them at reduced scale in one pass.
`thumbnail.output` folds both the monitor and output conversions and the
8-bit gamma table.

### Thumbnail runtime mapping

The thumbnail lifecycle is split across four entry points and two alternative
continuations, so one invocation cannot run all twelve nodes: `source.select`
and `source.embedded` are alternative roots, and `quickProcessImage()` and
`processImage()` are alternative successors of `thumbnail.scale`. With
`RT_STAGE_GRAPH=1` each entry point builds the canonical twelve-node spec,
connects it into a `StageGraph`, marks the nodes it owns dirty and asks
`StageGraph::plan()` for the actions:

| Entry point | Nodes it executes | Count |
|-------------|-------------------|-------|
| `loadQuickFromRaw()` | `source.embedded`, `thumbnail.decode` | 2 |
| `loadFromRaw()` | `source.select`, `raw.preprocess`, `raw.demosaic`, `raw.base` | 4 |
| `quickProcessImage()` | `thumbnail.scale`, `thumbnail.transform` | 2 |
| `processImage()` | `thumbnail.scale`, `thumbnail.transform`, `thumbnail.whitebalance`, `thumbnail.rgb`, `thumbnail.lab`, `thumbnail.output` | 6 |

The union is the full twelve-node graph; no node is faked. Every node an
entry point owns must be allowed to run: if `plan()` returns `ReuseOutput`
for one, the plan is rejected and the unchanged legacy sequence runs. The
remaining nodes are marked clean because their data is already held by the
`Thumbnail` (`thumbImg`) or belongs to the branch this invocation does not
take. The legacy sequence calls the same `stage*` blocks in the same order,
so the graph and the legacy path share one implementation.

## Parameter fields

`paramFields` lists `ProcParams` member names, not individual fields. The list
is provisional and deliberately a lower bound: a block that reads more of a
sub-struct than listed is not mis-specified, but a name that is not a
`ProcParams` member is a defect. The test enforces that with a whitelist built
from `procparams.h`.

## Parameter map and fingerprints

`rtengine/stage/stage_param_map.h/.cc` turns the export inventory into stable
parameter fingerprints. Each export descriptor gets one `ParamFingerprint`
built from a `ParamHasher` whose section label is the descriptor id, followed by
the `ProcParams` members that block reads. The mapping is conservative: whole
sub-structs are hashed, field by field, so a field the block ignores still
changes the fingerprint. A spurious cache miss is cheaper than a stale cache
entry.

API:

```cpp
bool has_export_fingerprint(const std::string& id);
ParamFingerprint export_fingerprint(const std::string& id,
                                    const procparams::ProcParams& params);
const std::vector<ExportParamMapEntry>& exportParamMap();
```

`exportParamMap()` is the review surface: descriptor id, consumed member names
and pipeline order. `has_export_fingerprint()` accepts only the twenty export
ids; preview, crop, thumbnail, monitor and viewport ids are rejected.

| Descriptor | `ProcParams` members |
|------------|----------------------|
| `source.select` | raw |
| `raw.preprocess` | raw, lensProf, coarse, dirpyrDenoise |
| `raw.demosaic` | raw, pdsharpening |
| `raw.base` | raw, wb, toneCurve, spot, dirpyrDenoise, filmNegative, retinex, dehaze, fattal, cg, icm |
| `analysis` | icm, toneCurve |
| `transform` | coarse, commonTrans, rotate, distortion, perspective, cacorrection, vignetting, gradient, pcvignette, lensProf, crop |
| `local.adjustments` | locallab, icm, toneCurve, raw, wb, dirpyrequalizer, dehaze, epd, rgbCurves, hsvequalizer, chmixer, colorToning |
| `rgb.proc` | toneCurve, rgbCurves, colorToning, blackwhite, chmixer, hsvequalizer, filmSimulation, icm |
| `lab.tools` | sh, labCurve, localContrast, vibrance, epd, softlight, icm, toneCurve |
| `lab.sharpening` | sharpening, sharpenEdge, sharpenMicro, impulseDenoise, defringe, icm |
| `lab.wavelet` | wavelet, dirpyrequalizer, icm, toneCurve |
| `working.trc` | toneEqualizer, icm, wavelet |
| `ciecam` | colorappearance, icm, toneCurve, raw, epd |
| `gamut` | cg, icm |
| `output.convert` | icm |
| `crop` | crop |
| `resize` | resize, crop, framing |
| `output.sharpen` | prsharpening |
| `framing` | framing, crop, resize |
| `output.icc` | icm, metadata |

### Refinements over the inventory

The inventory list above is a lower bound. These members were added from the
export entry point and the tool implementations:

- `icm` is a member of every stage that converts between working spaces or
  reads the working profile: `raw.base` (`convertColorSpace()` and Retinex),
  `analysis` (`firstAnalysis()` luminance weights), `local.adjustments`,
  `rgb.proc` (working-space matrix and DCP), `lab.tools` (vibrance),
  `lab.sharpening`, `lab.wavelet`, `working.trc`, `ciecam`, `gamut`,
  `output.convert` and `output.icc`.
- `transform` also hashes `crop`: post-crop vignetting needs the crop
  rectangle (`iptransform.cc:1030`, `:1103`).
- `local.adjustments` hashes the global members its spots borrow: working
  profile, CIE adaptation from `toneCurve` and `raw.expos`, the WB observer,
  `dirpyrequalizer`/`dehaze`/`epd` enable flags, and the RGB tool enables
  that decide negative-value handling (`iplocallab.cc:2648`, `:2788-2789`,
  `:3510`, `:15684-15687`, `:16688`, `:23816-23826`).
- `lab.tools` adds `icm` and `toneCurve` because `vibrance()` takes the
  working profile and `toneCurve.hrenabled` (`simpleprocess.cc:1750`).
- `lab.sharpening` adds `icm`; `ipsharpen.cc` reads the working profile.
- `lab.wavelet` adds `icm` and `toneCurve` (`ipwavelet.cc:223`, `:2017`).
- `working.trc` adds `wavelet`: the TRC completion path copies `WaveParams`
  for `complete_local_contrast()` (`simpleprocess.cc:1956-1960`).
- `ciecam` adds `toneCurve` and `raw` for adapting luminance
  (`simpleprocess.cc:2113-2114`), and `epd` for `EPDToneMapCIE()`
  (`improcfun.cc:1869`, `:5312-5328`).
- `resize` hashes `crop` and `framing`: `resizeScale()` reads the crop
  dimensions when resize applies to the cropped area, and the finishing path
  derives the scale from the framing data (`ipresize.cc:1034-1077`,
  `simpleprocess.cc:2223-2253`).
- `framing` hashes `crop` and `resize` because the framed size is derived from
  both (`simpleprocess.cc:2223-2357`).
- `crop` drops `cropGuide`: guides are an editor overlay and no export path
  reads them.

### Fields that are uncertain

These are recorded rather than hidden:

- `epd` is listed for `lab.tools` because the inventory says so and
  `EPDToneMap()` runs in that section (`simpleprocess.cc:1746`). It is also
  listed for `ciecam`, where `EPDToneMapCIE()` runs when CIE tone mapping is
  enabled. Both are conservative.
- `toneCurve` is listed for `analysis` although `firstAnalysis()` itself reads
  only `icm.workingProfile`. The inventory ties the analysis histogram to the
  auto-exposure parameters, so the whole member stays in the fingerprint.
- `dirpyrDenoise` is listed for `raw.preprocess` although only its `enabled`
  flag is passed to `preprocess()`. The whole member is hashed.
- `metadata` in `output.icc` changes the file metadata, not pixels. It is a
  real input of the sink and is kept.
- `resize`/`framing` and `crop` overlap deliberately: the finishing geometry
  reads all three members. A change to any of them moves more than one
  fingerprint. The mutation test records those cases as non-exclusive.

### Fingerprint format

The section label is added by `export_fingerprint()` from the table id, so a
hasher body cannot forget it or use a different string. Field order is the
order in the table and in the generated sub-struct hashers. The sub-struct
hashers cover every data member of the types they name; a member added to
`ProcParams` or to one of its sub-structs must be added to
`stage_param_map.cc` at the same time, otherwise fingerprints would silently
ignore it. The mutation test proves the current lists, not future additions.

### Verification

```bash
tools/build-rt.sh cpu -j 6 --ccache --stage-modules
cmake --build build/cpu-phaseB-param-map --target stage-param-map-test -j 6
build/cpu-phaseB-param-map/rtengine/stage-param-map-test
```

Result:

```text
stage-param-map-test: PASS
```

The OpenCL configuration compiles (`tools/build-rt.sh opencl -j 6 --ccache`)
and the same target builds and passes in `build/opencl-phaseB-param-map`. The
test checks:

1. every canonical export descriptor has a fingerprint, and non-export ids
   (preview, crop, thumbnail, monitor, viewport, unknown) do not;
2. repeated calls and an equal `ProcParams` copy give identical fingerprints;
3. the table has twenty unique ids in pipeline order, all inside the export
   spec, and every field name is a `ProcParams` member;
4. a mutation table covering WB, demosaic, transform, local adjustments,
   RGB/tone curve, wavelet, CIE CAM, gamut, output profile, crop, resize,
   framing and the remaining descriptors changes the named descriptor;
5. an exclusive field changes no other descriptor, and a non-exclusive field
   is only required to change its own;
6. members outside a descriptor's list do not change it.

The sub-struct hashers read 55 `ProcParams` types and 1567 data members.

## Reuse and consumer semantics

- `recompute`: must run for every request (sources and final sinks).
- `reuse-output`: the block has a cacheable output buffer (raw load,
  demosaic, base finish, analysis, output conversion, crop, resize, framing,
  thumbnail decode and scale).
- `reuse-input`: the block may alias its input when inactive (geometry,
  local adjustments, RGB, Lab, sharpening, wavelet, working TRC, CIE CAM,
  gamut, thumbnail transform/white balance/RGB/Lab).
- Export is `Exact`; preview, crop and thumbnail are `Interactive`. This is
  the Phase A contract; promoting an interactive block to a cached exact
  result needs parity evidence before it becomes default.

## Blocks that are folded, not separate

These are real processing calls that the canonical graph represents inside a
neighbouring block. They are recorded here so the gap is explicit rather than
silently dropped.

- `imgsrc->captureSharpening()` (`improccoordinator.cc:1750`) runs inside
  `raw.demosaic`, because it is a post-demosaic raw operation.
- Global Retinex (`simpleprocess.cc:415-427`, `improccoordinator.cc:1788-1797`)
  runs on the raw source before `getImage()` and is folded into `raw.base`.
- Dehaze and Fattal tone mapping (`simpleprocess.cc:1152-1153`,
  `improccoordinator.cc:2292-2293`, `dcrop.cc:798-799`) run after
  `firstAnalysis()` in the current order. The export chain therefore carries
  them as the prologue of `transform`, not inside `raw.base`, so a single
  topological order reproduces the implementation's pixel order. The
  provenance moved with them: `transform` lists `dehaze` and `fattal`.
- `imgsrc->convertColorSpace()` (`dcrop.cc:693`) is the working-space
  conversion performed for the crop and is folded into `raw.base`.
- `ipf.moyeqt()` saturation statistics and `ipf.calc_ref()` local-adjustment
  reference values are folded into `rgb.proc` and `local.adjustments`
  respectively; they are derived data, not image blocks.
- The preview soft-proof `lab2rgb()` is folded into `monitor.output`, in the
  same method as the monitor conversion. A separate soft-proof sink is not
  modelled.
- Embedded-thumbnail rotation exceptions for Leaf `.mos`, Mamiya `.mef` and
  Phase One `.iiq` are folded into `thumbnail.decode` / `thumbnail.transform`.
- On the embedded branch the raw container load is folded into
  `source.embedded`: `source.select` is a separate root and is not executed,
  because the container is opened only to reach the embedded JPEG.
- `Thumbnail::firstAnalysis()`, `dehaze()`, `ToneMapFattal02()`, the
  lens-geometry `ipf.transform()` and `Lab_Local()` run between the colour
  conversion and `rgbProc()` in `stageRgb()`. The canonical thumbnail path has
  no `analysis`, `transform` or `local.adjustments` node, so they are folded
  into `thumbnail.rgb`. Only the coarse rotate/flip is `thumbnail.transform`.
- The raw fallback histogram and auto white-balance analysis in
  `stageRawBase()` is folded into `raw.base`; there is no `analysis` node on
  the thumbnail path.
- Inspector mode (`ThumbnailInspectorMode::RAW` and the
  `RAW_IF_NOT_JPEG_FULLSIZE` fallback) loads a full-resolution image, which is
  outside the reduced-domain graph. It keeps the legacy path and executes no
  graph node.
- Output ICC attachment and metadata (`simpleprocess.cc:2387-2410`) are one
  block, `output.icc`, even though they are two calls.
- Post-crop vignette and gradient are computed by `iptransform.cc:1023` and
  `:1030`, so they are parameters of `transform`, not separate blocks.

No processing block found in the four entry points is missing from the
inventory. The only intentional omissions are derived statistics, metadata
application and the soft-proof sink listed above.

## Execution boundaries (Phase B)

`rtengine/simpleprocess.cc` now has one private `ImageProcessor` method per
export node. The legacy sequence (`stage_finish()` and `legacy_export()`) and
the graph executor (`graph_pipeline()`) call the same methods, so the export
pipeline has one implementation and the graph only changes which code invokes
it.

| Node | Method |
|------|--------|
| `source.select` | `stage_source_select()` |
| `raw.preprocess` | `stage_raw_preprocess()` |
| `raw.demosaic` | `stage_raw_demosaic()` |
| `raw.base` | `stage_raw_base()`, calling `stage_raw_base_source()`, `stage_denoise()`, `stage_raw_color()` |
| `analysis` | `stage_analysis()` |
| `transform` | `stage_transform()` |
| `local.adjustments` | `stage_local_adjustments()` |
| `rgb.proc` | `stage_rgb_proc()` |
| `lab.tools` | `stage_lab_tools()` |
| `lab.sharpening` | `stage_lab_sharpening()` |
| `lab.wavelet` | `stage_lab_wavelet()` |
| `working.trc` | `stage_working_trc()` |
| `ciecam` | `stage_ciecam()` |
| `gamut` | `stage_gamut()` |
| `crop` | `stage_crop()` |
| `resize` | `stage_resize()` |
| `output.sharpen` | `stage_output_sharpen()` |
| `output.convert` | `stage_output_convert()` |
| `framing` | `stage_framing()` |
| `output.icc` | `stage_output_icc()` |

Blocks that remain folded inside a node, and why:

- `raw.base` is three calls in order: the base image and tone-curve analysis,
  denoise, and the source colour conversion plus source gamut compression.
  The split exists so the fast export can run denoise after its early resize.
- `analysis` allocates the working histogram as well as running
  `firstAnalysis()`. The allocation moved ahead of the source gamut
  compression that ends `raw.base`; it allocates a LUT and reads no pixels, so
  the pixel order is unchanged.
- `crop` also computes the framing geometry (`FramingArgs` and
  `FramingData`), because the Lab crop and the resize targets depend on it
  before the output conversion. The `framing` node draws the frame.
- `resize` performs the Lab resize only. When the resize method is Nearest,
  the resize runs on the gamma-applied RGB buffer, which exists only after
  `lab2rgbOut()`, so that pass is part of `output.convert`.
- `output.convert` also forces the B&W output and releases the Lab image.

## Determinism

`dumpPipelineSpec()` walks the node and edge vectors in insertion order,
which is fixed by `definitionFor()`. Node order, edge order, roots and sinks
are therefore stable, and the test asserts that two dumps of the same path are
byte-identical. The dump is Graphviz DOT with a comment line per node carrying
domain, consumer, reuse and parameter fields.

## Verification

Build and test:

```bash
tools/build-rt.sh cpu -j 6 --ccache --stage-modules
cmake --build build/cpu-phaseA-inventory --target stage-pipeline-test -j 6
build/cpu-phaseA-inventory/rtengine/stage-pipeline-test
```

Result:

```text
stage-pipeline-test: PASS
```

Both `cpu-*` and `opencl-*` configurations compile and the test passes in
both. The test checks, per path:

1. unique node ids;
2. every edge endpoint is a known node and no edge is duplicated;
3. node inputs mirror the incoming edges;
4. one root, except the thumbnail path with its embedded root and raw
   fallback; every root has no input and every sink no output;
5. every export block is present, and export-only blocks are absent from
   preview;
6. preview, crop and export share the common core ids;
7. the thumbnail path has a reduced domain, an embedded branch and the raw
   fallback;
8. the graph is acyclic, every node is reachable from a root and every node
   can reach a sink;
9. every parameter field is a known `ProcParams` member;
10. the dump is deterministic and contains every node and edge.

Node counts: export 20, preview 14, crop 15, thumbnail 12.

The thumbnail runtime mapping is exercised separately:

```bash
cmake --build <build-dir> --target thumbnail-stage-test -j 6
<build-dir>/rtengine/thumbnail-stage-test <raw> [pp3] [height] [out-dir]
tools/perf/run-suite.sh --build-dir <build-dir> --image <raw> --thumbnail-graph
```

The harness runs `loadQuickFromRaw()` + `quickProcessImage()` and
`loadFromRaw()` + `processImage()` once with the legacy sequence and once with
`RT_STAGE_GRAPH=1`, compares the 8-bit output byte for byte and fails if a
plan falls back. Result on the CPU build: `PASS` for both branches, with all
four plans active; hashes and the image profile are recorded in
`STAGE_RUNTIME.md`.

Phase B corrected the export chain to the implementation's order (see
"Execution boundaries" above). The node and edge counts, roots, sinks and
reuse modes are unchanged, and `stage-pipeline-test` passes in the CPU and
OpenCL configurations. The export graph parity evidence is in
`devnotes/perf/STAGE_RUNTIME.md`.
