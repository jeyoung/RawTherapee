# Stage pipeline specification

This record defines the canonical descriptor inventory and dependency graph for
the four engine pipelines: export, preview, crop and thumbnail. It is the
Phase A artefact for the stage runtime: the graph and its block boundaries are
specified and tested here, before any production pipeline is rewired.

Vocabulary: the typed `EditorRequest` (facts and dirty blocks) is the only
request type. `M_*` names appear below only in historical migration mappings,
in quoted code that was removed, and in the retirement guard; none is a current
rule. The mask-era sentences are kept because they explain why the derivation
looks the way it does.

The specification is data only. `rtengine/stage/stage_pipeline.cc` builds it and
`rtengine/stage/tests/stage_pipeline_test.cc` proves its shape. All four engine
paths execute this graph through `StageGraph::plan()`; the hand-ordered
sequences and their `RT_STAGE_GRAPH`/`RT_PREVIEW_GRAPH` switches have been
removed (see `STAGE_RUNTIME.md`).

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
| Invalidation | `rtengine/editorrequest.h`, `stage/stage_dirty_blocks.*` | typed request facts and blocked ids |

## Canonical block catalogue

Ids are stable. A block that a path does not run is absent from that path's
spec: `buildPipelineSpec()` returns the rooted subgraph, not the union.

`Device` records how far a block has moved onto the stage-ABI device path:

| Value | Meaning |
|-------|---------|
| `yes` | the block has its own stage-ABI device node |
| `partial` | a composite block whose dominant pixel passes are device nodes; the host-side residual is named with the block record in `STAGE_RUNTIME.md` |
| `no` | host-only: the block has no device node |

A block's runtime node name can differ from its id; the name is given where it
does. `partial` never means verified: it records device execution of the named
sub-nodes and the residual beside it, not parity of the composite.

| Id | Domain (export/preview/crop/thumbnail) | Consumer | Reuse | Role | Device |
|----|----------------------------------------|----------|-------|------|--------|
| `source.select` | Full/Full/Full/Full | path | recompute | select the frame to process | no |
| `source.embedded` | -/-/-/Reduced | interactive | recompute | select the embedded JPEG | no |
| `raw.preprocess` | Full/Full/Full/Full | path | reuse-output | raw black/white, hot/dead pixel, CA, flat field | no |
| `raw.demosaic` | Full/Full/Full/Full | path | reuse-output | demosaic and capture sharpening | no |
| `raw.base` | Full/Full/Roi/Reduced | path | reuse-output | `getImage()`, spot removal, denoise, film negative, Retinex, source-side gamut | no |
| `analysis` | Full/Full/Roi/- | path | reuse-output | histogram and auto-exposure analysis | no |
| `transform` | Full/Full/Roi/- | path | reuse-input | rotation, distortion, perspective, CA, vignetting, gradient, post-crop vignette, dehaze, Fattal | yes |
| `local.adjustments` | Full/Full/Roi/Reduced | path | reuse-input | local lab / selective editing | no |
| `rgb.proc` | Full/Full/Roi/Reduced | path | reuse-input | RGB curves, tone curve, colour toning, B&W, channel mixer, HSV, film simulation | yes |
| `lab.tools` | Full/Full/Roi/Reduced | path | reuse-input | shadows/highlights, Lab curves, local contrast, vibrance, EPD, Lab-grid colour toning | no |
| `lab.sharpening` | Full/-/Roi/- | path | reuse-input | impulse denoise, defringe, microcontrast, sharpening | no |
| `lab.wavelet` | Full/Full/Roi/Reduced | path | reuse-input | directional pyramid equaliser and wavelet | no |
| `working.trc` | Full/Full/Roi/- | path | reuse-input | soft light, tone equaliser and working-TRC conversion | no |
| `ciecam` | Full/Full/Roi/- | path | reuse-input | CIE CAM 02 | no |
| `gamut` | Full/Full/Roi/Reduced | path | reuse-input | working-space gamut compression | yes |
| `output.convert` | Full/-/-/- | exact | reuse-output | Lab to output RGB and output profile conversion | yes |
| `crop` | Full/-/-/- | exact | reuse-output | crop | no |
| `resize` | Full/-/-/- | exact | reuse-output | resize (`lanczos3`) | yes |
| `output.sharpen` | Full/-/-/- | exact | reuse-input | post-resize sharpening | no |
| `framing` | Full/-/-/- | exact | reuse-output | framing and frame drawing | yes |
| `output.icc` | Full/-/-/- | exact | recompute | attach output ICC and metadata | no |
| `monitor.output` | -/Full/-/- | interactive | recompute | Lab to monitor RGB | yes |
| `viewport.output` | -/-/Roi/- | interactive | recompute | Lab to monitor RGB for the viewport | yes |
| `thumbnail.decode` | -/-/-/Reduced | interactive | reuse-output | decode the embedded JPEG | no |
| `thumbnail.scale` | -/-/-/Reduced | interactive | reuse-output | scale to thumbnail height | yes |
| `thumbnail.transform` | -/-/-/Reduced | interactive | reuse-input | coarse rotate and flip | yes |
| `thumbnail.whitebalance` | -/-/-/Reduced | interactive | reuse-input | apply white balance to the thumbnail | yes |
| `thumbnail.rgb` | -/-/-/Reduced | interactive | reuse-input | thumbnail RGB processing | partial |
| `thumbnail.lab` | -/-/-/Reduced | interactive | reuse-input | thumbnail Lab tools, working TRC, CIE CAM and gamut in one pass | no |
| `thumbnail.output` | -/-/-/Reduced | interactive | recompute | Lab to 8-bit thumbnail with gamma | yes |

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

`StageGraph` drives the inventory through the executor; there is no other
sequence. Each block is one private
`ImProcCoordinator::stage*()` method, so the inventory and the executor cannot
drift apart:

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

The node entry is the plan: a preview node executes only when the dirty set
names it. A node body still decides *how much* of its output the request
allows, and it reads the typed `EditorRequest` facts for that rather than the
mask: `raw.base` rebuilds the base for one class of requests, re-prepares
retinex for another and re-applies spots for a third. `monitor.output`
executes at its legacy site, after the crop render, so the monitor profile
cache refresh it follows still feeds the crop viewport.

The producer side is typed too. Tools name a `RequestIntent`;
`make_request()` builds the typed kind and facts, and the event registry stores
the request. The event key is the request identity and the history message, so
two actions that plan identically but carry different payload facts stay
distinct. The integer `newEvent(action, message)` overload is deleted.

Three statements that were unconditional in the hand-written pass stay in the
caller, never in a node: the `oprevi = orig_prev` working-pointer reset, the
local-adjustment damping floor, and the frame-count and image-type notices.
The Color/Coloropp `markColorHlrDemosaic()` augmentation also runs there so the
plan sees the typed request the pass will use.

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
`rtengine/rtthumbnail.cc`; each is the only implementation of that step, called
by the graph executor.

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
`processImage()` are alternative successors of `thumbnail.scale`. Each entry
point builds the canonical twelve-node spec,
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
for one, the plan is rejected and the process aborts. The
remaining nodes are marked clean because their data is already held by the
`Thumbnail` (`thumbImg`) or belongs to the branch this invocation does not
take. The graph executor calls the same `stage*` blocks, so there is one
implementation.

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
| `analysis` | icm, toneCurve, dirpyrDenoise |
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
- `dirpyrDenoise` is a member of `analysis`: the crop builds the
  automatic-denoise analysis from the denoise configuration, and enabling that
  denoise leaves `denoiseAnalysisValid` false, so only the parameter delta can
  dirty the analysis node directly (see the request facts below).
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
- `sharpening` and `prsharpening` omit five `SharpeningParams` members that
  the constructor leaves uninitialised and that no code reads:
  `deconvAutoRadius`, `deconvCornerBoost`, `deconvCornerLatitude`,
  `psf_kernel` and `psf_iterations`. Hashing them made the fingerprint depend
  on stack contents, so the two sharpening export fingerprints changed when
  the defect was found. The regression table pins their new values, a test
  writes the five members to prove they are excluded, and another constructs
  `ProcParams` over two differently poisoned buffers to check the remaining
  fields. Restore them when the constructor initialises them and a processing
  path reads them.

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

1. every canonical export descriptor has an export fingerprint, and ids
   outside the export path do not;
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

### Preview and crop tables

The parameter map also covers the preview and crop graphs. The preview table
has fourteen descriptors and the crop table fifteen. Both reuse the export
definition of every shared block, so `raw.base`, `transform`,
`local.adjustments`, `rgb.proc`, `lab.tools`, `lab.wavelet`, `working.trc`,
`ciecam` and `gamut` have one field list on all paths. The crop path adds
`lab.sharpening`; preview omits it. `monitor.output` (preview) and
`viewport.output` (crop) are separate display sinks that currently read `icm`.

API:

```cpp
bool has_preview_fingerprint(const std::string& id);
ParamFingerprint preview_fingerprint(const std::string& id,
                                     const procparams::ProcParams& params);
bool has_crop_fingerprint(const std::string& id);
ParamFingerprint crop_fingerprint(const std::string& id,
                                  const procparams::ProcParams& params);
const std::vector<StageParamMapEntry>& previewParamMap();
const std::vector<StageParamMapEntry>& cropParamMap();
```

The monitor profile, monitor intent and the soft-proof and gamut-check
switches are application or GUI state, not `ProcParams`, so they cannot enter
these fingerprints. The crop ROI rectangle is a request input for the same
reason. Both are carried by `RenderStateKey` (see below). The crop table
still hashes the crop rectangle through `transform`, because post-crop
vignetting reads it.

The test checks every descriptor and table order against
`buildPipelineSpec()`, neutral-parameter stability, a mutation matrix across
the parameter families of both paths, and a regression guard that pins the
twenty neutral-parameter export fingerprints. It runs as
`stage-param-map-test` in `build/cpu-phaseF-preview-map`.

### Render state key

`rtengine/stage/stage_render_state.h` defines `RenderStateKey`, the explicit
render state that the parameter tables cannot represent. It is a plain value
type with no pointers and no `ImageSource`: preview scale, preview and full
dimensions, the crop/ROI rectangle with its skip and processing border, the
high-detail decision and what has already been computed for it,
`sourceGeneration`, `rgbSourceModified`, the monitor profile name and intent,
soft proof, gamut check, `sharpMask`, the fast-preview switches, whether the
pass is the initial rough preview, the request kind, the previous-demo base
and twelve request facts.

`RequestKind` has five values (`ParamsOnly`, `RepaintOnly`, `FullCrop`,
`FullPreview`, `HighQuality`). `previousDemoBase` is stored as an `int32_t`
that mirrors `PreviousDemoBase` (`Unknown`, `Fast`, `Sidecar`). Both value
sets are part of the persisted format and must not be renumbered.

`RenderStateKey::fingerprint()` hashes every field, in declaration order, into
a `ParamHasher` stream labelled `render-state`. It uses only the typed
overloads, so struct padding, host byte order and pointer values cannot affect
the result.

The preview full-frame graph consumes the key field by field through
`rtengine/stage/stage_dirty_blocks.h`: a changed field group dirties the blocks
that read it, and the set is merged with the parameter-fingerprint difference
before the graph plan runs. No cache key uses the fingerprint yet. The bitset
still drives the viewport fast path, GUI notification and the per-node gates,
and it carries the crop raw-reload request, but it no longer selects the
scheduled nodes.

`stage-render-state-test` proves identical states agree, every field changes
the fingerprint when changed alone, every request kind is distinct,
`sourceGeneration` is significant, equal strings at different addresses agree,
and the fingerprint equals a field-by-field canonical stream. It also pins the
published fingerprint `7d18d6df20ff6afdb471515b8cdae2b8` for the populated
sample key; update it only with a deliberate format change.

#### Request facts and their aspects

Twelve fields carry inputs that neither `ProcParams` nor an earlier field
expresses. Eight are mapped to a `RenderStateAspect`; four are deliberately
exempt.

Mapped to Source, alongside `sourceGeneration` and `rgbSourceModified`:
`sourceRotateDegree`, `sourceSensorType`, `sourceFullWidth`,
`sourceFullHeight` and `sourceFrameCount`. They describe the source that the
generation identifies, so they cannot widen a dirty set beyond the reload the
generation already schedules.

Mapped to Derived, a new aspect between high detail and display:
`dirPyrDenoiseExpComp`, the exposure compensation the source contributes to
the automatic-denoise analysis; `denoiseAnalysisValid`, whether the
coordinator holds a usable analysis for this request; and `spotBufferLive`,
whether a spot buffer exists that the next base pass restores. The row is
`{raw.base, analysis}` on both the preview and the crop path. `raw.base` is
named explicitly because the canonical edge is `raw.base -> analysis`, so the
closure from the analysis never pulls the base. `spotBufferLive` is base
state, not an analysis input: it changes the next base pass whether or not the
analysis moves.

`spotBufferLive` also drives a standing rule: whenever the current key reports
a live buffer, `derive_dirty_blocks()` and `dirtyBlocksFromRenderState()`
schedule `raw.base` whatever the deltas say. The Derived aspect reports the
transition that creates or drops the buffer; the rule reports the state that
changes the next base pass. The preview coordinator's own insertion of
`raw.base` when it holds a spot buffer (`improccoordinator.cc`) is redundant
now and is retained until the gate-removal phase.

The four exempt facts:

- `spotPending`: only the false->true transition matters, and the spot
  parameter delta already carries it; the true->false transition has no
  parameter delta and no block consumes the cleared flag.
- `deferredAnalysisMask`: a scheduling fact the coordinator absorbs before it
  builds the key, published from an advisory atomic snapshot, so it cannot
  order a dirty set.
- `cropGuideFingerprint`: names an editor overlay that no stage consumes; its
  later job is to suppress a repaint, not to dirty a block.
- `viewportMoved`: already represented by `RequestKind::RepaintOnly` on the
  crop; mapping it again would double-count the repaint.

`dirpyrDenoise` is now a member of the `analysis` fingerprint and field list
(`hashAnalysis`), alongside `icm` and `toneCurve`. The crop builds the
automatic-denoise analysis from the denoise configuration, but enabling the
automatic denoise leaves `denoiseAnalysisValid` false->false, so no request
fact moves and the analysis node was not directly dirtied before the change;
it was reached only through the `raw.base` closure.

### Dirty-block derivation (Phase F)

`rtengine/stage/stage_dirty_blocks.h` is the single derivation that both the
crop viewport and the preview coordinator use. It turns two inputs into the
set of blocks a pipeline must execute: a parameter-fingerprint difference per
descriptor, a render-state difference per field group, their union, the
coupled Lab group, the caller's residents, and an explicit forward closure
over the pipeline edges. It reads neither the legacy mask nor pixels and
executes nothing. `devnotes/perf/STAGE_RUNTIME.md` records the two
integrations and the shadow evidence; this section records the rules and the
per-path tables.

| Function | Meaning |
| --- | --- |
| `stage_fingerprints(path, params)` | Ids and fingerprints of every canonical block of the path, once |
| `changed_render_state_aspects(a, b)` | Field groups that differ between two render states |
| `blocks_for_render_state(path, aspect)` | Blocks one changed aspect names, per path |
| `request_rebuilds_path(kind)` | Whether a request kind asks for a whole-path rebuild |
| `coupled_block_groups(path)` | Blocks that share one gate and one buffer seed |
| `resident_blocks(path)` | Blocks the caller executes on every invocation |
| `close_dirty_blocks(path, blocks)` | Forward closure over the pipeline edges |
| `derive_dirty_blocks(inputs)` | The derivation: params, state, groups, residents, closure |
| `compare_dirty_shadow(legacy, derived)` | `missing` (unsafe) and `extra` (over-approximation) |
| `format_block_ids(blocks)` | One-line log rendering, `[a b c]` |

`derive_dirty_blocks()` is a pure function of its inputs. A caller keeps the
previous `BlockFingerprints` and `RenderStateKey` instead of a `ProcParams`
copy, so an update hashes the current parameters once (15 descriptors for the
crop path, 14 for the preview) rather than twice.

### Rules

1. **Parameter delta.** A block is directly dirty when its fingerprint differs
   from the previous snapshot. Equal parameters give an empty set.
2. **Render-state delta.** A changed `RenderStateKey` group dirties the blocks
   its aspect names. The groups are geometry, source, high detail, derived,
   display, editor and request. Every key field joins exactly one group or is
   listed in the header as deliberately exempt; joining two groups would
   report one change twice and, through their different block rows,
   over-dirty the path.
3. **Rebuild request.** A `FullCrop`, `FullPreview` or `HighQuality` request
   dirties the whole path. A request is absolute, not a delta: it says "this
   pass rebuilds the path", so it cannot be expressed as an input change. The
   preview coordinator takes the kind from the typed `EditorRequest`, whose
   adapter maps the request family once, because that family stands for work
   no parameter or render-state field describes: the high-quality family is
   `HighQuality`, the initialisation, line-denoise and HDR families are
   `FullPreview`, and an ordinary pass is `ParamsOnly`. No retired mask bit is
   read at runtime.
4. **Coupled group.** The Lab chain shares one `laboCrop -> labnCrop` seed and
   one gate. Seeding any member seeds the whole group: crop
   `lab.tools`, `lab.sharpening`, `lab.wavelet`, `working.trc`, `ciecam`,
   `gamut`; preview the same without `lab.sharpening`.
5. **Residents.** `raw.base`, `transform` and `viewport.output` on the crop,
   and `monitor.output` on the preview, are executed on every invocation.
   They are part of the execution set but do not seed
   the closure: their bodies are still mask-gated, so a resident alone does
   not regenerate anything.
6. **Forward closure.** Every consumer of a directly dirty block is dirty,
   because the path carries one in-place image and no intermediate cache yet.
   The closure stops at the sinks and never reaches a producer.
7. **Canonical output.** Every returned id is a canonical descriptor of the
   requested path, in pipeline order, without duplicates, so two sets compare
   with `==`.

### Compatibility forms

The preview coordinator was written against a first, standalone draft of this
unit. It is ported onto the shared derivation through thin adapters with the
same answers: each returns the execution set (residents and closure included)
and fills `DirtyBlocks::ids`, `valid` and `reason`.

| Adapter | Shared functions it runs |
| --- | --- |
| `snapshotParams(params, path)` | `stage_fingerprints()` |
| `dirtyBlocksFromParams(previous, current, path)` | `snapshotParams()` then `derive_dirty_blocks()` |
| `dirtyBlocksFromSnapshot(snapshot, current, path)` | `stage_fingerprints()` then `derive_dirty_blocks()` |
| `dirtyBlocksFromRenderState(previous, current, path)` | `changed_render_state_aspects()`, `blocks_for_render_state()`, `resident_blocks()`, `close_dirty_blocks()` |
| `allDirtyBlocks(path)` | the canonical node list |
| `mergeDirtyBlocks(a, b)` | set union |
| `closeOverGraph(blocks, path)` | `close_dirty_blocks()` |

The state adapter reads two inputs that the aspect tables cannot express: a
changed `previousDemoBase` maps to the high-detail blocks, because the preview
base feeds the raw core, and a live spot buffer schedules `raw.base` directly.
The request aspect is likewise evaluated per kind rather than per row, as
rule 3 describes. There is no mask equivalence form in this unit; the mask map
remains a separate description and the shadow comparison compares plans, not
tables.

### Per-path tables

Changed render-state aspect to blocks:

| Aspect | Crop | Preview |
| --- | --- | --- |
| geometry | `raw.base`, `analysis`, `transform`, `local.adjustments`, `rgb.proc`, `lab.tools`, `lab.sharpening`, `lab.wavelet`, `working.trc`, `ciecam`, `gamut`, `viewport.output` | every node |
| source | `source.select`, `raw.preprocess`, `raw.demosaic`, `raw.base` | `source.select`, `raw.preprocess`, `raw.demosaic`, `raw.base` |
| high detail | `raw.preprocess`, `raw.demosaic`, `raw.base`, `analysis`, `transform` | `source.select`, `raw.preprocess`, `raw.demosaic`, `raw.base`, `analysis`, `transform` |
| derived | `raw.base`, `analysis` | `raw.base`, `analysis` |
| display | `viewport.output` | `monitor.output` |
| editor | `lab.sharpening` | `source.select`, `raw.preprocess`, `raw.demosaic`, `raw.base`, `analysis`, `transform` |
| request | `viewport.output` | see rule 3 |

Two rows are deliberately wider on the preview path. Its high-detail row
includes `source.select` because the preview selects the frame per pass before
the high-detail preprocessing it belongs to; the crop never reloads the
full-frame source for an ROI repaint. Its editor row is the union of the
sharpening-mask toggle (`raw.demosaic`, whose planes the mask replaces) and the
FAST, full-resolution and rough-preview switches (the raw core); one aspect
cannot name a different set per switch, so the wider answer costs an inert
call and never skips a block.

The geometry row covers the crop rectangle as well as the preview scale, so a
preview crop change re-renders the whole preview. That is the conservative
answer: the crop viewport's row is the tighter crop set because the crop path
has no full-frame source reload in its geometry change.

`StagePath::Export` and `StagePath::Thumbnail` do not carry a
`RenderStateKey`; the state adapter returns the whole path and marks the result
invalid, which is the fail-closed answer for a caller that passes one anyway.

### Crop wiring

`Crop::update()` builds the current snapshot from `parent->params` and the
coordinator state (ROI rectangle, skip, borders, high-detail flags, source
generation, monitor configuration, sharpening mask) and derives the set before
any stage body can touch the parameters. It stores the snapshot again after
the pass, because several stage bodies write corrected values back into the
parameters (automatic white balance, automatic denoise) and those corrections
belong to the state the buffers hold, not to the next update's inputs.

The one thing the crop still takes from the request is the raw-reload
decision: the init, line-denoise and HDR families ask for the destructive raw
stage to be reloaded, which no parameter fingerprint can express. The request
kind is carried into the render state as `RequestKind::FullCrop`, so the
derivation treats the pass as a rebuild instead of silently skipping the
reload. Every other input is derived.

The derived set is the execution-set authority. There is no
`RT_CROP_DIRTY` mode and no runtime mask baseline. `crop-stage-test --sequences`
applies the typed request of each edit to the stage bodies, then compares the
incremental crop with a fresh render of the final state. For the
destructive raw/core bodies the typed work order is the execution-set
authority; the dirty-block set still schedules the downstream nodes.

The `todo` coalescing and the plan validation in `planCropGraph()` are
unchanged. A rejected plan aborts instead of running a full legacy loop, and
the graph's dirty flags come from the derived set.

### Dirty-block evidence

`crop-stage-test --sequences` drives one parameter or viewport change inside a
live session for each scenario, applies the typed request of the edit to the
stage bodies, and compares the incremental crop with a fresh render of the
final state.

Image: the public 10.2 MP PEF sample `colorspace_flowers.pef` (listed in
`devnotes/opencl/testdata.manifest`), 100% viewport 960x640,
`OMP_NUM_THREADS=1`. The run reports `33 plans executed, 0 plan fallbacks,
33 dirty derivations` and `AE=0` for display and output-space images in every
scenario and in the static fit/100%/zoomed states. The stage-coverage run also
passes with denoise, local adjustments, CIE CAM, gamut and the Lab chain
active.

The focused harness:

```bash
cmake --build <build-dir> --target crop-stage-test -j 6
<build-dir>/rtengine/crop-stage-test <raw> [pp3] [out-dir] [--stages] \
    [--cbdl=aft] [--no-ciecam]
tools/perf/run-suite.sh --build-dir <build-dir> --image <raw> --crop-graph
```

It loads the image twice, runs the full-frame preview once per copy, then the
detailed crop for fit, 100% and zoomed/panned viewports through the derived
execution set. It compares display and output-space images byte for byte and
requires one executed plan per state, one dirty derivation per update, and no
plan rejection. Result on CPU and OpenCL builds: `AE=0` for every state and
profile.

#### Raw/core gate retirement

The crop destructive bodies were converted to the typed work order at
`500ee68c1` and the legacy raw/core gates were retired at `56157e8b8`. At
`56157e8b8` every crop and preview raw/core body runs from the derived order;
no site reads the mask for whether that body runs. The delta-block derivation
and the downstream Lab/transform/monitor gates are unchanged.

Removed crop predicates (all tested directly in `stageSourceSelect`,
`stageAnalysis` and `stageRawBase`):

```cpp
(todo & (M_INIT | M_LINDENOISE | M_HDR)) != 0
(todo & M_SPOT) != 0
spotCrop != nullptr
```

Removed preview predicates (the batch gate functions in
`improccoordinator.cc`):

```cpp
(todo & M_PREPROC) != 0 || (!highDetailPreprocessComputed && highDetailNeeded)
(todo & M_RAW) != 0 || (!highDetailRawComputed && highDetailNeeded)
    || (toneCurve.hrenabled && !isColor && rgbSourceModified)
    || (!toneCurve.hrenabled && isColor && rgbSourceModified)
(todo & (M_RAW | M_CSHARP)) != 0 && pdsharpening.enabled
(todo & (M_INIT | M_LINDENOISE | M_HDR)) != 0
(todo & (M_RETINEX | M_SPOT)) != 0
(todo & (M_RETINEX | M_INIT)) != 0 && retinex.enabled
(todo & M_SPOT) != 0 && spot.enabled && !spot.entries.empty()
spotprev != nullptr
```

The request facts that remain are deliberate boundary folds, not gates:
`RequestKind` from the init, line-denoise, HDR and high-quality families, and
`spotPending` from the spot family. The HDR/Fattal block (`hdrIntent`) and the
demosaic body's internal sub-gates read typed facts: they decide how much of a
node runs, not whether the raw/core body is entered. The transform, Lab and
monitor gates are untouched.

The spot buffer clear is now covered by `spotRestore`: a live buffer is named
whether the tool is still configured (restore) or not (clear). That is why the
site no longer reads `M_SPOT`.

`RT_WORK_ORDER=0`/`off` no longer restores a mask. The caller derives with no
usable history, so every parameter-applicable raw/core item is named: a
conservative superset, safe and slower. The unit test pins the derivation and
the preview sweep runs the conservative order against the exact one.

#### Retirement evidence

The conversion commit kept the read-only comparison active. On the crop
harness it reported `typedOnly=0` on every pass of every state, `legacyOnly=0`,
and `unprobed` only on the crop-only state where the plan omits the source and
denoise stages that the order did not name. The preview sweep reported
`typedOnly=0` on all 14 states; its `legacyOnly` entries were the documented
redundant cases (working-profile demosaic and the high-detail fit
transition).

At `56157e8b8`, image 10.2 MP PEF `colorspace_flowers.pef`, 100% viewport
960x640, `OMP_NUM_THREADS=1`:

| Run | Plans | Fallbacks | Derivations | AE |
| --- | --- | --- | --- | --- |
| `crop-stage-test --sequences` | 33 | 0 | 33 | 0, all states |
| `crop-stage-test --stages` | 6 | 0 | 6 | 0, all states |
| `--sequences --extra-states` (spot, denoise, fattal) | 42 | 0 | 42 | 0, all states |
| `--sequences --extra-states` with `RT_WORK_ORDER=off` | 42 | 0 | 42 | 0, all states |

The PPM outputs of the conversion run and the retirement run are byte-identical
for all 31 files, including the spot, denoise, fattal and geometry sequences.
`preview-work-order-sweep.sh`: 14 states, `AE=0` for `preview.tif` and
`work.tif`, kill switch 4 enabling and 3 disabling values, and no comparison
line remains. Single-threaded 16-bit TIFF export with `RT_WORK_ORDER` on and off
is `AE=0`; `thumbnail-stage-test` passes.

Timing on the Intel Core i7-3770 host, 40 samples each, same image and control
edit, `OMP_NUM_THREADS=1`:

| Harness | Before (p50 / p95) | After (p50 / p95) |
| --- | --- | --- |
| crop exposure edit | 251.7 / 264.2 ms | 244.7 / 260.0 ms |
| preview exposure edit | 321.4 / 329.0 ms | 320.1 / 328.3 ms |

Both changes are within run-to-run noise. The CPU and OpenCL configurations
compile, and the 14 stage unit tests pass in both trees.

#### Request-fact change evidence

The request-fact mapping and the analysis fingerprint change were checked on
the CPU build unless stated:

- `stage-abi-test`, `stage-cache-test`, `stage-device-test`,
  `stage-dirty-blocks-test`, `stage-graph-test`, `stage-indexer-test`,
  `stage-param-hash-test`, `stage-param-map-test`, `stage-parity-test`,
  `stage-pipeline-test`, `stage-render-state-test`, `stage-store-test` and
  `viewport-admission-test`: 13 PASS.
- `crop-stage-test --sequences`: PASS, `33 plans executed, 0 plan fallbacks,
  33 dirty derivations`, `AE=0` in the static fit/100%/zoomed states and in
  every sequence state.
- `thumbnail-stage-test`: PASS embedded and raw.
- `crop-stage-test --stages`: PASS with denoise, local adjustments, CIE CAM,
  gamut and the Lab chain active, `6 plans executed, 0 plan fallbacks`.
- The CPU and OpenCL configurations both compile. The OpenCL run rebuilt the
  engine and linked `stage-dirty-blocks-test`, `stage-param-map-test`,
  `stage-pipeline-test` and `stage-render-state-test`.

The preview bench is not usable evidence for this change. The harness captured
only two samples because its hard-coded slider coordinates miss on this
layout, so no timing claim is made.

## Retired mask maps

The legacy request mask and its equivalence data are deleted. There is no
`refreshmap.*`, no `stage_mask_map.*`, no `M_*` constant and no compatibility
mask on a request. The intents in `rtengine/editorrequest.h` name the request
families; `make_request()` builds the typed facts; the stage bodies read the
facts.

The static `ProcEventCode` identities keep their meaning as event identities.
`rtengine/editorrequest_registry.cc` records the intent of each static event as
a compact range table, and a producer event created by
`ProcEventMapper::newEvent()` stores its typed request directly. History and
deduplication are unchanged.

`editorrequest-test` runs the reintroduction guard over the engine, GUI and
tool sources. It fails on a retired unit, helper or mask constant, and on an
integer `newEvent` action.

## Preview typed work order

The preview raw-chain gates run from the typed order by default. The order is
derived once per pass by `rtengine/stage/stage_work.cc`. The legacy `M_*` gates
are retired, so there is no runtime shadow probe: the exact expected order of
every parameter case is recorded in `rtengine/stage/tests/stage_work_test.cc`. `RT_WORK_ORDER` is the kill switch:
any value leaves the typed order enabled, while `0` and `off`
(case-insensitive) derive the order with no usable history, so it names every
parameter-applicable item. Unset is the default and enables the exact order.

### Sweep

`tools/perf/preview-work-order-sweep.cc` drives the real
`StagedImageProcessor`/`ImProcCoordinator` through one initial pass and then
one edit per state, waiting for each full-frame pass to settle. It copies the
`RT_PREVIEW_DUMP` files (`preview.tif`, `work.tif`, `preview.json`) after every
state. The viewport fast path is disabled inside the harness
(`RT_VIEWPORT_FAST=0`) so every state is a full-frame pass. The preview scale is
4, the profile is the default, and the fit state uses the FAST preview demosaic
so the 100% crossing is a real high-detail transition.

The dumps are 8-bit image data in 16-bit containers: the preview path produces
8-bit RGB, and `preview.tif` and `work.tif` widen it without adding precision.
`AE` is therefore a byte comparison, not a 16-bit-precision comparison.

```bash
export RT_BUILD_DIR="$PWD/build/cpu-perf-graph-workflags"
tools/perf/preview-work-order-sweep.sh \
    build/rt-testdata/colorspace_flowers.pef \
    build/test-scratch/work-order-sweep
```

The driver builds the target, probes the kill switch (with `RT_WORK_SHADOW`
unset, so an ambient value cannot fail the probe), runs the sequence with
`RT_WORK_ORDER=off` and with the default, and compares every state with
`magick compare -metric AE`. `RT_WORK_SHADOW` is a diagnostic marker only: it
changes no engine behaviour and is kept so a recorded run can state which mode
it used. The state list comes from the run log and a state directory missing on
either side fails the run; the driver never passes on the strength of the
directories that happen to exist. It writes the two run logs and the state
dumps under the output directory.

Timing samples use the same harness. The script documents the flag and runs
the exact commands itself:

```bash
export RT_BUILD_DIR="$PWD/build/cpu-perf-graph-workflags"
tools/perf/preview-work-order-sweep.sh \
    build/rt-testdata/colorspace_flowers.pef \
    build/test-scratch/work-order-timing --timing=10

# Equivalent, if the harness is invoked directly:
OMP_NUM_THREADS=1 env -u RT_WORK_SHADOW RT_WORK_ORDER=off \
    "$RT_BUILD_DIR/rtengine/preview-work-order-sweep" \
    build/rt-testdata/colorspace_flowers.pef \
    build/test-scratch/work-order-timing/off --timing=10
OMP_NUM_THREADS=1 env -u RT_WORK_ORDER -u RT_WORK_SHADOW \
    "$RT_BUILD_DIR/rtengine/preview-work-order-sweep" \
    build/rt-testdata/colorspace_flowers.pef \
    build/test-scratch/work-order-timing/on --timing=10
```

### States and parity

The matrix covers the required families, with the working-profile change
driven twice (once with retinex enabled and once without) so the retinex buffer
dependency is exercised in both directions, and with two retinex buffer inputs
edited while the tool is already enabled. Sixteen settled passes ran: the
initial pass plus fifteen states. The initial pass is a counted state, not an
excluded warm-up: it is dumped, compared and listed like every other state.

| # | State | Action | Preview AE | Work AE |
|---|-------|--------|-----------:|--------:|
| 0 | initial | load | 0 | 0 |
| 1 | spot-add | enable one spot entry, `SPOTADJUST` | 0 | 0 |
| 2 | spot-remove | clear and disable the spot tool, `SPOTADJUST` | 0 | 0 |
| 3 | retinex-on | `retinex.enabled = true`, `DEMOSAIC` | 0 | 0 |
| 4 | retinex-colorspace | `Lab` -> `HSLLOG` with retinex on, `DEMOSAIC` | 0 | 0 |
| 5 | retinex-gamma | `gammaretinex` `none` -> `low` with retinex on, `DEMOSAIC` | 0 | 0 |
| 6 | workprofile-retinex-on | `Rec2020` -> `ProPhoto`, `DEMOSAIC` | 0 | 0 |
| 7 | retinex-off | `retinex.enabled = false`, `DEMOSAIC` | 0 | 0 |
| 8 | workprofile-retinex-off | `ProPhoto` -> `Rec2020`, `DEMOSAIC` | 0 | 0 |
| 9 | filmneg-on | `filmNegative.enabled = true`, `ALLNORAW` | 0 | 0 |
| 10 | filmneg-off | `filmNegative.enabled = false`, `ALLNORAW` | 0 | 0 |
| 11 | capsharp-on | enable capture sharpening, `DEMOSAIC` | 0 | 0 |
| 12 | capsharp-off | disable capture sharpening, `DEMOSAIC` | 0 | 0 |
| 13 | zoom-100 | `setMainPreviewFullResolution(true)` | 0 | 0 |
| 14 | zoom-fit | `setMainPreviewFullResolution(false)` | 0 | 0 |
| 15 | exposure | `expcomp += 0.7`, `AUTOEXP` | 0 | 0 |

Comparison: ImageMagick 7.1.1-43 Q16, `magick compare -metric AE` on the
8-bit dumps carried in 16-bit containers. `AE=0` for both `preview.tif` and
`work.tif` in all sixteen states.

States 4 and 5 are the regression guard for the retinex buffer inputs. With
the pre-fix predicate they were the only two states that differed: `AE=633744`
for both `preview.tif` and `work.tif`, in both cases because the exact order
skipped the demosaic body and left the buffers describing the previous colour
space or gamma while `retinex()` consumed them with the new parameters. With
the fix they are `AE=0`.

The AE comparison proves that the exact order does not skip a pixel-affecting
body: the conservative run names every parameter-applicable item, so any body
the exact order dropped would show up as a difference. It cannot by itself
prove `typedOnly=0`, because an extra body can reproduce identical pixels.
That check is the unit oracle (`stage-work-test`): every case is compared with
the action its editor sends, and an item the typed order names but the action
does not set fails the test. The retired shadow's last `legacyOnly` record is
kept below because the arguments have not changed; the initial pass was
counted there too:

| State | legacyOnly | Resolution |
|-------|------------|------------|
| workprofile-retinex-off | `rawDemosaic` | Retinex is already disabled and no demosaic input moved; the legacy `DEMOSAIC` event re-demosaics identical planes. The typed order skips the body. Pixels are identical (AE=0); redundant work. |
| zoom-fit | `sourceSelect rawPreprocess` | The fit request re-selects the already-loaded frame and re-preprocesses identical raw and lens-profile inputs. The demosaic **is** named in this direction because the fit pass switches the demosaic mode to FAST. Pixels are identical (AE=0); redundant work. |

### Derivation corrections found by the sweep

The first clean run failed on the retinex state with a crash and on the
high-detail drop with non-zero AE; the capture sharpening disable was corrected
in the same change because it is the same in-place-body case. The fixes are in
`derive_preview_work()`. The list is not exhaustive: the body-input audit below
found two further gaps that the sweep could not see, because it only toggled
tool enable flags.

1. **Retinex plane preparation.** `retinexPrepareBuffers()` lives at the end
   of the `raw.demosaic` body, and `retinex()` in `raw.base` consumes the
   buffers. The typed `rawDemosaic` predicate had no retinex input, so
   enabling retinex skipped the preparation and crashed in
   `RawImageSource::retinex()` on an unallocated `conversionBuffer[2]`.
   Disabling retinex also left its in-place output in the planes. The new
   `retinexPlanes` term names `rawDemosaic` when retinex is newly enabled,
   when it is disabled (only a fresh demosaic removes the output), and when
   the working profile moves while it is enabled (the buffers are converted
   with the working-space matrix). A bare retinex strength edit dirties the
   retinex family, reuses the buffers and still does not name the body.
2. **Capture sharpening disable.** Capture sharpening writes the demosaiced
   planes in place. The typed predicate named the body only while the tool was
   currently enabled, so disabling it kept the sharpened planes. It now names
   the body when either the previous or the current tool state is enabled,
   matching the legacy `DEMOSAIC` action; the fixed run is `AE=0` for the
   disable state and every later state.
3. **High-detail drop.** Below 100% the caller substitutes the FAST demosaic
   method; at 100% the configured method runs. The typed predicate only named
   pending high-detail work, which covers the gaining direction. Dropping the
   decision changed the pixels without naming the body. The `highDetailMode`
   term names `rawDemosaic` in both directions; the crossing request names the
   raw source for exactly that reason.

### Body-input audit

The sweep compares runs, so it only catches a predicate that is wrong for a
state the sweep drives. This audit instead enumerates what each executed body
reads and checks that the item's predicate, or an item it implies, names every
input. `named` means the predicate covers it; `gap, fixed` is a defect found by
this audit and repaired here; `shared gap` is an input that neither the typed
order nor the legacy gate names, where naming it would be a typed-only run and
therefore a divergence from the legacy path.

| Item | Inputs read by the executed body | Named by | Verdict |
|------|----------------------------------|----------|---------|
| `sourceSelect` (preview) | `raw.bayersensor.imageNum` | `changed("source.select")` | named |
| `sourceSelect` (crop) | `params.coarse` (`getCoarseBitMask`), `params.dirpyrDenoise` (noise curves), crop geometry | `reload` | shared gap: the legacy gate was the same `M_INIT \| M_LINDENOISE \| M_HDR` reload, so a `coarse`-only `ALLNORAW` edit left the bit mask stale on both paths |
| `rawPreprocess` | preprocess raw fields, `lensProf` vignetting fields, `coarse`, high-detail need | `changed("raw.preprocess")`, `source`, `hdPre` | shared gap: `coarse` feeds the vignetting map, but the legacy `M_PREPROC` family excludes `M_RAW` and no `coarse` action carries `M_PREPROC` |
| `rawDemosaic` | demosaic raw fields, `raw.*.border`, `pdsharpening`, `retinexcolorspace`, `gammaretinex`, `gam`, `slope`, `icm.workingProfile`, high-detail decision, `rgbSourceModified`, `toneCurve` recovery fields | the demosaic block, `captureSharpening`, `source`, `preprocess`, `hdRaw`, `highDetailModeChanged`, `hlr*`, `retinexPlanes` | **gap, fixed:** the four retinex buffer inputs were not named, so a colour-space or gamma edit reused buffers built for the previous one (states 4 and 5, `AE=633744` before the fix) |
| `rawBaseRebuild` | `wb`, `toneCurve`, `dirpyrDenoise`, `filmNegative` and `params.raw`, `cg`, `icm` (working space), `spot.entries`, `currWB`, request reload | the eight families, `icm-without-output`, `preprocess`/`demosaic` forward edges, `rawReload` | named; `retinex` is in the family list because `retinex()` rewrites the planes before the base |
| `retinexPrepare` | every `retinex` member, `icm.workingProfile`, `toneCurve.hrenabled`, the raw planes and the buffers | `family(retinex)`, `icm-without-output`, `preprocess`, `demosaic`, `retinexHighlight` | **gap, fixed:** `retinex()` reads `Tc.hrenabled` for the Lab-mode gamut control and the editor sends `ALLNORAW` (which carries `M_INIT`), so the legacy gate re-ran it and the typed order did not |
| `spotApply` | `spot.entries`, `currWB`, `icm`, request bit | `spotPending`, `spotsConfigured && family(spot)` | named; a white-balance edit carries `M_SPOT`, so the request fact covers the `currWB` dependency |
| `spotRestore` | live buffer, configured spots, base-rebuild outcome | `spotBufferLive`, `spotApply && !base` | named |
| `analysisRun` | `icm`, `toneCurve`, `dirpyrDenoise` via `firstAnalysis()` | the rawBaseRebuild predicate (analysis is folded into raw.base) | named |
| `denoiseRun` (crop) | `dirpyrDenoise`, `cropSkip` | `reload`, `dirpyrDenoise.enabled`, `cropSkip == 1` | named; the preview has no denoise body at all |

Two inputs stay out on purpose, both pre-existing legacy behaviour rather than
divergence: `str` crossing zero, which only toggles the preparation's forward
gamma (the `RETINEX` action carries no `M_RAW` on either path), and the crop's
`coarse`-derived bit mask. Both are `shared gap` rows above.

### Execution-gate retirement (step 4)

The body gates no longer read the execution mask. `updatePreviewImage()` keeps
no `int todo`; `renderCrops()`, `Crop::update()`, `CropStageContext`, the
preview timer and the dump take the typed request. Every conversion keeps the
sub-step semantics: a node is entered by the plan and decides inside it which
work the request allows. The gates, their class and their replacement are:

| Gate | Site | Class | Old condition | New condition |
|------|------|-------|---------------|---------------|
| crop render | `renderCrops()` | body | `todo & (M_MONITOR\|M_RGBCURVE\|M_LUMACURVE)` | `monitorIntent \|\| rgbProcIntent \|\| lumaCurveIntent` |
| PC-vignette transform | `updatePreviewImage()` | scheduling | `todo & M_CROP`, then `\|= TRANSFORM` | `cropIntent`, then `markCropTransform()` |
| panning requeue | `updatePreviewImage()` | scheduling | `todo \|= M_MONITOR` | `markViewportMoved()` |
| base refresh | `updatePreviewImage()` | scheduling | `todo \|= M_INIT\|M_HDR\|M_MONITOR` (+`M_SPOT`) | `markBaseRefresh(spotChanged)` |
| deferred absorb | `absorbPendingDeferred()` | payload | `todo \|= deferredExecutionPayload(...)` | `absorbDeferredExecution(blocks)` |
| Color/Coloropp demosaic | pass setup | payload | `pass.todo \|= DEMOSAIC` + manual facts | `markColorHlrDemosaic()` |
| demosaic sub-step | `stageRawDemosaic()` | body | `pass.todo & M_RAW` | `rawIntent` |
| capture sharpening | `stageRawDemosaic()` | body | `pass.todo & (M_RAW\|M_CSHARP)` | `rawIntent \|\| captureSharpenIntent \|\| demosaicRebuilt` |
| retinex preparation | `stageRawDemosaic()` | body | `pass.todo & M_RAW` | `rawIntent` |
| demosaic rebuild widening | `stageRawDemosaic()` | payload | `pass.todo \|= M_INIT\|M_CSHARP` | `demosaicRebuilt`, applied after the node loop by `markDemosaicRebuilt()` |
| output-profile refresh | post-node | body | `pass.todo & M_MONITOR` | `monitorIntent` |
| crop force | `Crop::update()` | scheduling | `if (forcedFull) todo = ALL` | removed; `forcedFull` and the request facts already admit the same work |
| timer / dump / trace | diagnostics | diagnostic | `int todo` | request kind and identity |

`monitorIntent` is the exact monitor family, not `monitorOutputRequest`: a
spot or white-balance request renders `monitor.output` but does not name the
monitor transform, and the crop gate and profile refresh follow the family.
`rawIntent` and `captureSharpenIntent` are payload-shaping facts like
`luminanceIntent`, and all of them participate in `identity()`. No deferred
payload, mask adapter or crop-todo oracle remains.

Only one gate changed shape, and only in how it is expressed:
`captureSharpening` names the local `demosaicRebuilt` fact explicitly instead
of relying on the widened `pass.todo`. Every other gate is a one-to-one
substitution, and the post-node widening stays at its original position so it
cannot leak into the transform node of the same pass. The dead
`M_LINDENOISE` block in `stageRawBase()` is inside a comment and is marked
retired; no compiled read remains.

#### Step-4 evidence

Host CPU build, `OMP_NUM_THREADS=1`, image 10.2 MP PEF
`colorspace_flowers.pef`:

| Check | Result |
| --- | --- |
| preview sweep, 16 states | PASS, `AE=0` for `preview.tif` and `work.tif` in every state |
| `crop-stage-test --sequences --extra-states` | PASS, 48 plans, 0 fallbacks, 48 dirty derivations, `AE=0` |
| `crop-stage-test --stages` | PASS, 6 plans, 0 fallbacks, 6 dirty derivations, `AE=0` |
| viewport fast path | PASS, 5 hits, 5 deferred analyses, 1 initial fallback, 18 images `AE=0` |
| node cache, on vs off | PASS, 16 states `AE=0`; on `hits=0 misses=5 refused=11 inserts=5` |
| 16-bit TIFF export, work order on vs off | `AE=0` |
| `thumbnail-stage-test` | PASS embedded and raw |
| both builds | `cpu-perf-graph-retire` and `opencl-perf-graph-retire` compile; 16 stage tests plus `editorrequest-test` PASS in both |
| control exposure, 10 samples | before p50 383.7 ms / p95 390.9 ms, after p50 373.5 ms / p95 377.0 ms |
| viewport fast path, 10 samples | before crop p50 58.0 ms (n=9), after 55.0 ms (n=9); settled p50 636.1 vs 635.6 ms |

### Timing

Ten samples per side on the same image, default profile, preview scale 4, fit
state, `OMP_NUM_THREADS=1`, host CPU build. The host is an Intel Core
i7-3770 (4C/8T). The control exposure edit rebuilds the base in both modes, so
no speed-up is expected; the Clamp-OOG toggle is the case where the typed
order skips the legacy preprocess and demosaic.

| Case | Mode | p50 (ms) | p95 (ms) |
|------|------|---------:|---------:|
| exposure `expcomp += 0.7` | off | 318.6 | 319.6 |
| exposure `expcomp += 0.7` | on | 317.8 | 318.9 |
| Clamp-OOG toggle | off | 510.3 | 513.2 |
| Clamp-OOG toggle | on | 315.3 | 317.9 |

The Clamp-OOG p50 falls from 510.3 ms to 315.3 ms (-38.2%); p95 falls from
513.2 ms to 317.9 ms (-38.1%). The exposure control is unchanged within noise
(-0.3% p50), which is the expected result because both modes rebuild the base.

### Builds and tests

`tools/build-rt.sh cpu -j 6` and `tools/build-rt.sh opencl -j 6` complete.
All 15 `stage-*` test executables plus `editorrequest-test` and
`viewport-admission-test` pass in both build trees:
`stage-abi-test`, `stage-cache-test`, `stage-node-cache-test`,
`stage-device-test`, `stage-residency-test`, `stage-graph-test`,
`stage-indexer-test`, `stage-param-hash-test`, `stage-render-state-test`,
`stage-param-map-test`, `stage-dirty-blocks-test`, `stage-work-test`,
`stage-pipeline-test`, `stage-store-test`, `stage-parity-test`,
`editorrequest-test` and `viewport-admission-test`. `stage-work-test` pins the
switch parser (`nullptr`, empty,
`1`, `yes` enable; `0`, `off`, `OFF` disable) and the corrected predicates
(retinex planes and buffer inputs, capture-sharpening disable, high-detail
drop).

#### Re-verification after the retinex buffer-input fix

Host CPU build, image 10.2 MP PEF `colorspace_flowers.pef`,
`OMP_NUM_THREADS=1`:

| Check | Result |
| --- | --- |
| `stage-work-test` | PASS, including the four retinex buffer-input cases, the retinex-off direction and the highlight-recovery toggle |
| `preview-work-order-sweep.sh` | PASS, 16 states, `AE=0` for `preview.tif` and `work.tif` in every state; the pre-fix predicate fails states 4 and 5 with `AE=633744` |
| `crop-stage-test --sequences` | PASS, 33 plans executed, 0 plan fallbacks, 33 dirty derivations, `AE=0` in every state |
| `crop-stage-test --stages` | PASS, 6 plans executed, 0 plan fallbacks, 6 dirty derivations, `AE=0` |
| Single-threaded 16-bit TIFF export, `RT_WORK_ORDER` on vs off | `AE=0` |
| `thumbnail-stage-test` | PASS embedded and raw |
| Both configurations | `tools/build-rt.sh cpu -j 6` and `tools/build-rt.sh opencl -j 6` compile; 16 stage test executables plus `editorrequest-test` PASS in both trees |

## Node-output cache of the preview raw.base tier

Status: opt-in (`RT_NODE_CACHE`), default off. The work-order conversion is
unchanged; the cache is a second consumer of the typed order and of the pass
fingerprints.

### Problem

The preview destroys and rebuilds `orig_prev` in place. A pass that names
`rawBaseRebuild` pays `getImage()`, white balance, spot removal, film negative,
the colour-space conversion, gamut compression and `firstAnalysis()` again,
even when the pass input is byte-identical to an earlier pass (a repeated
request, a toggle returned to its previous value, a redundant redraw). The
node-output tier makes that repeat explicit and measurable.

### Key

`stage::NodeCacheKey` is an exact value, not a 64-bit digest:

| Component | Covers |
|-----------|--------|
| `node` | The canonical node id of the tier (`preview.raw.base`), so two nodes with equal inputs cannot share an entry. |
| `render` | The 128-bit `RenderStateKey::fingerprint()`: preview scale and allocated size, crop/ROI rectangle, skip and borders, the high-detail decision and the work done for it, source generation and source facts, a modified RGB source, monitor and soft-proof configuration, editor switches, request kind, the base the preceding pass used, the denoise/spot facts, the deferred analysis mask, the crop-guide fingerprint and the viewport-moved fact. The whole fingerprint is used; a subset could go stale when a field is added. |
| `blocks` | Every canonical block fingerprint of the preview pass, keyed by block id, from the same `ParamSnapshot` the work order was derived from. Covers every `ProcParams` member a block reads, including the raw families that reach the base through preprocess/demosaic. `ParamSnapshot` is a `std::map`, so the entry order is the map's lexicographic id order, not pipeline order; the same snapshot always produces the same order, which is all exact equality needs. |
| `work` | The producing work items restricted to the tier (`rawBaseRebuild`, `retinexPrepare`, `spotApply`, `spotRestore`, `analysisRun`). Two passes that ran different bodies are different producers even with equal parameters. |
| `backend` | The 128-bit `nodeCacheBackendFingerprint()`: the acceleration switch and, when accelerated, the selected device's index and identity strings. Different kernels can round the same input differently, so a device change that leaves every other input equal must still miss. |
| `width`, `height`, `planes`, `format` | Output geometry and layout, so a scale or layout change can never match. |

The key is encoded field by field into `StageKey::stage` (fixed-width
hexadecimal fingerprints, unit-separated ids), so `StageCache` equality is
exact string equality. `StageKey::params` is the hash of that encoding and is
carried for the container; equality compares the full encoding as well, so no
decision rests on the digest alone. There is no resident-stamp digest API.

Deliberately excluded, and why it is safe:

- **Process pointers, `Imagefloat` allocation addresses.** They change on
  every reallocation without changing pixels.
- **`currWB`, `vhist16`, `denoiseInfoStore.valid`.** They are outputs of the
  same node and are stored in the entry's auxiliary payload instead of being
  hashed. The payload carries its own version; version 2 adds the no-balance
  flag described below.
- **The spot buffer.** Its content is an input to the tier, but it is not a
  function of the parameters: a spot edit/undo sequence can leave a live
  buffer whose pixels came from an earlier edit. It is neither keyed nor
  stored; reuse is refused while `spotPending` or `spotBufferLive` is set.
- **Wall clock and thread count.** The tier is process-local, and the
  deterministic stage bodies do not depend on either.

#### Pre-body snapshot limitation

The key is built from `pass.fingerprints` and `pass.renderState` before any
body runs. A body that writes parameters back (the white-balance
normalisation, the automatic methods) leaves the live parameters different
from the key the tier is stored under. The entry is correct for the inputs it
names, but the next pass carries the written-back parameters, fingerprints a
different key and cannot reach it. The write-back is still required for
correctness; this is a utilisation limit, not a parity risk. A parameter set
that is written back by every pass (for example a non-Camera automatic white
balance) therefore gets little or no node-cache reuse.

### Reuse decision

`nodeCacheReuseAllowed(work, render)` refuses the tier when:

- the typed order names any raw source producer (`sourceSelect`,
  `rawPreprocess`, `rawDemosaic`, `retinexPrepare`): the plan would regenerate
  the planes the base is built from; or
- `spotPending` or `spotBufferLive` is set: the live spot buffer can change
  the output without a keyed input.

The spot refusal is exact rather than an approximation. Storing the buffer
would require hashing its full-preview bytes into the key and copying them
into every entry, which roughly doubles the budget for a state that is rare
compared with ordinary parameter edits. Refusing the state is the safer
trade: the common non-spot preview still reuses, and a spot-configured image
recomputes every base pass. The call site passes the same predicate to both
the probe and the store, so a refused state can neither serve nor create an
entry.

`nodeCacheSubstitutionAllowed()` requires `rawBaseRebuild` in the order. A
substitution replaces the end-of-node tier, so every downstream node must
rerun from it. The dirty-block derivation seeds the whole path for a rebuild
request and closes forward over the consumers for a base delta, so a scheduled
base rebuild always reruns the chain. A pass that enters `raw.base` for spot
maintenance alone keeps the downstream intermediate in the shared buffer, and
a substitution there would corrupt the in-place chain; it is refused.

On a hit the whole destructive branch is skipped, including the first
analysis and the listeners it drives. The listener values are unchanged by
definition of the key; the GUI already holds them. The restored auxiliary
state covers `currWB`, `vhist16` and `denoiseInfoStore.valid`.
`params->wb.temperature`/`green` are written back from the restored `currWB`
when white balance is enabled, because the rebuild branch normalises them in
place and the next pass would otherwise fingerprint a different state.

White balance has a sentinel: the body assigns the default `ColorTemp()` when
`wb.enabled` is false, and `RawImageSource::getImage()` branches on
`getTemp() < 0` to restore the raw pre-multipliers. The five-argument
constructor clamps that sentinel to 1500 K / 0.02, so the payload stores
`wb_no_balance` and `rawBaseTierWhiteBalance()` rebuilds the exact sentinel.
It also verifies every stored field and returns false when a constructor
would clip, leaving the caller to recompute instead of rendering an
approximation.

### Budget, eviction, counters, switch

- `RT_NODE_CACHE` is the switch. Unset is the default and disables the cache.
  `0` and `off` (case-insensitive) disable it; every other value, including the
  empty string, enables it. With it off, `NodeCache` allocates nothing and
  every call is a no-op: behaviour and memory are identical to the base commit.
  `NodeCache::allocationCount()` (backed by `StageCache::allocationCount()`) is
  the test seam that proves the disabled path never reaches an allocation.
- `RT_NODE_CACHE_MB` is the budget in MiB. Default **256**, valid range
  **1..8192**; a missing, malformed or out-of-range value falls back to the
  default. The budget is per coordinator instance (one per editor tab); an
  entry is about 7.9 MiB for the 10.2 MP preview at scale 4. The auxiliary
  state shares the entry and the budget.
- Eviction is true least-recently-used. `probe()` and `restore()` refresh an
  entry (the probe's `peek()` and the copy's `fetch()` both move it to the
  front); `contains()` is introspection and does not. Insertion and a budget
  shrink evict from the least-recently-used end.
- `RT_NODE_CACHE_STATS` (any value) prints one summary at process exit:
  `node-cache: budget= used= hits= misses= refused= inserts= rejects=
  evictions= bytes-in= bytes-evict=`.
  `hits`/`misses` are probes, `refused` are probes the work-order or spot
  rules declined, `inserts`/`rejects` store attempts, `evictions` budget
  removals. An entry larger than the whole budget is rejected, not admitted.

### Tests

- `stage-cache-test`: auxiliary payload round-trip, counters, reset, true LRU
  for `peek()`/`get()`/`fetch()`, `contains()` not refreshing recency (the LRU
  case fails under FIFO), allocation accounting, oversized rejection,
  replacement is not an eviction, budget shrink eviction, refusal accounting.
- `stage-node-cache-test`: switch parser; key equality and collision
  resistance for a different source generation, preview width, crop
  rectangle, preview scale, spot pending/live, block fingerprint, block id,
  producing work order, backend, node id, output width and format; the reuse
  and substitution decisions including the spot refusal; the call-site guard;
  repeated-pass restore with `AE=0`; true LRU through `probe()` and
  `restore()`; the WB sentinel round-trip, the clipped-state refusal and a
  byte-level cached-versus-uncached comparison; the disabled path's zero
  allocation count; malformed, oversized and corrupt auxiliary payloads.

### Evidence

Host CPU build (`build/cpu-perf-graph-cache`), GCC, generic target,
`OMP_NUM_THREADS=1`, image 10.2 MP PEF `colorspace_flowers.pef`, preview
scale 4. All figures are from the fixed code, after the spot refusal, the
sentinel-aware white-balance restore, true LRU and the OpenCL-in-key change.

| Check | Result |
|-------|--------|
| `stage-cache-test` / `stage-node-cache-test` | PASS in the CPU and OpenCL trees |
| Preview sweep, 16 states, `RT_NODE_CACHE` off vs on | all 16 states `AE=0` for `preview.tif` and `work.tif`; cache on `hits=0 misses=5 refused=11 inserts=5` |
| Repeated exposure, WB enabled, 10 samples | `hits=7 misses=3 refused=1 inserts=3`; a 4-state dump run is `AE=0` in every state, including the hit state |
| Repeated exposure, WB disabled, 10 samples | `hits=8 misses=2 refused=1 inserts=2`; a 4-state dump run is `AE=0` in every state, including the hit state |
| `crop-stage-test --sequences` | 33 plans, 0 fallbacks, 33 derivations, `AE=0` |
| `crop-stage-test --stages` | 6 plans, 0 fallbacks, 6 derivations, `AE=0` |
| 16-bit TIFF export, `RT_NODE_CACHE` off vs on | `AE=0` (regression check only: the export path never constructs a `NodeCache` and prints no node-cache summary) |
| `thumbnail-stage-test` | PASS embedded and raw (the thumbnail path does not use the tier) |
| `tools/build-rt.sh cpu` / `opencl` (`--ccache`) | both trees compile; 16 stage test executables plus `editorrequest-test` PASS in both |

The preview sweep produced no hits at all: every state either missed a fresh
key or was refused by the work-order or spot guard, so all 16 `AE=0` results
are the recomputed path. The repeated exposure runs below are the cases that
hit.

The WB-disabled end-to-end figures come from a local copy of the sweep
harness with `params.wb.enabled = false` before the initial pass. The
committed unit test proves the same property without the harness: the sentinel
round-trips field for field and the simulated no-WB transform is byte-identical
for the cached and uncached state, while the clamped reconstruction differs.

### Timing

Ten repeated exposure samples per side on the same image and profile, after
the initial pass. The `on` figures are the mean of the hit samples; the first
two or three samples are misses while the two alternating states are filled.

| Case | off p50 (ms) | on hit mean (ms) | samples | gain |
|------|-------------:|-----------------:|--------:|-----:|
| WB enabled | 321.8 | 300.7 | 10 / 10 (7 hits) | -6.5% |
| WB disabled | 321.6 | 300.2 | 10 / 10 (8 hits) | -6.7% |

The safe refusals do not reduce these gains because the repeated exposure
states are not spot states. A spot-configured image loses preview node-cache
reuse entirely: the spot states are refused and recompute, so they get the
cache-off timing. That is the deliberate cost of an exact rather than an
approximate decision.

Known limits: the tier is process-local; the preview canvas keeps one live
buffer, so only a scheduled rebuild can be substituted; every spot state is
refused; parameters written back by the body make the entry unreachable; and
listeners are not re-fired on a hit. The next step is to give nodes private
buffers, which removes the canvas constraint and lets the store serve without
the rebuild-closure guard.

## Node device residency (POCL CPU)

The graph has a device path: a node can run on an OpenCL device with its
inputs uploaded to device buffers and its output read back into the host
buffer the graph already owns. The path is opt-in; the default is the host
program. The host execution statements are unchanged, but the default path is
not free: `stage::run` now resolves the node's policy first. `node_device()`
reads two environment variables, checks a cached override flag and returns CPU
without building a reason string or taking the override-map lock in the common
case. `select_node_device()` adds the diagnostics for callers and tests.

### First device node

`resize` is wired through the `lanczos3` stage ABI node (`ImProcFunctions::
Lanczos` -> `stage::run`). It is the only node that already had a stage-ABI
implementation and an existing kernel with a documented parity history
(resize 0.5x/1.5x at 0-1 LSB), so it is the smallest honest device node. The
host implementation and the OpenCL kernel precompute the same weights with the
same code and perform the separable dot products in the same order, so the
results agree to rounding; the measured difference is zero.

The classic backend still owns the accelerated resize: when the user enables
OpenCL, `ImProcFunctions::Lanczos` calls `opencl::resizeLanczos` before the
stage runtime, so the device node is reached only when that path declines.

### Selection and switches

| Control | Effect |
|---------|--------|
| default | CPU; the host program runs |
| `RT_STAGE_OPENCL=1` | opt in; nodes with a device implementation are dispatched to the device queue |
| `RT_STAGE_OPENCL=0` | kill switch; even an explicit device request resolves to CPU |
| `RT_STAGE_OPENCL_DISABLE=1` | kill switch, independent of the opt-in value |
| user preference, preview | the coordinator sets `useOpenCL && openCLMode == 1`; mode 0 is export-only and does not allow the device in a preview pass |
| user preference, batch | `simpleprocess` sets `useOpenCL`; export is accelerated in both OpenCL modes, and `--gpu=off` sets `useOpenCL=false`, which now also disables the graph device |
| `stage::select_node_device(node, device)` | explicit per-node request; returns the resolved device, whether an implementation exists and the reason for a refusal |
| `stage::set_node_device_override()` | per-node override for tests and embedding; the kill switches still win |

The policy is per-thread, like `opencl::setStageAllowed()`, so a preview pass
and a batch export in one process cannot overwrite each other's setting. A node
without a device implementation reports `implemented=false` and runs on the
host program. A node whose device allocation is refused (budget) or whose
launch fails also runs on the host, so a device problem never fails a render.

### Buffers, budget and events

- The residency keys a device allocation by the host buffer address, which is
  the node output it belongs to. A repeated node with the same address and
  shape reuses the allocation; a shape change replaces it; `release(key)`
  drops it; the context destroys the rest.
- The budget defaults to 1 GiB and is set by `RT_STAGE_DEVICE_BUDGET_MB`.
  Out-of-range values clamp to 16-65536 MiB and a non-numeric value keeps the
  default. `StageResidencyStats` reports budget, used, peak, allocations,
  reuses, evictions and refusals.
- Over budget: idle entries (no other reference) are evicted least-recently-
  used first. If a single buffer still does not fit, the request is refused
  with a reason, counted as a refusal, and the node runs on the host.
- U8 is packed RGB and must be a single plane. The layout helper refuses
  multi-plane U8, which is the only shape where it would disagree with
  `stage::row_bytes()`; the engine views only produce single-plane U8.
- Events carry the device that actually executed. The CPU fallback completes
  before `submit()` returns (`asynchronous=false`, `device=CPU`). A device
  submission returns `Running` with a completion handle (`device=OPENCL`,
  `asynchronous=true`); `poll()` queries the driver and can report `Failed`
  early, and `wait()` blocks, reads the output back and only then reports
  `Complete`. An event that executed nothing, such as a compiled host program
  offered to the device queue, reports `device=NONE`.
- `StageContext::execute()` fills a `StageExecution { device, asynchronous }`
  from the completed event. Callers and the profile record use it instead of
  the selection, so a queue that falls back to the host is never labelled as a
  device execution.

### Evidence

Builds: `build/cpu-perf-graph-opencl` and `build/opencl-perf-graph-opencl`,
GCC, generic target, `--ccache`. Device runs restrict the ICD to `pocl.icd`
(`OCL_ICD_VENDORS`), set `RT_OPENCL_ALLOW_CPU=1` and use
`OMP_NUM_THREADS=1`; the selected device reports `cpu-ivybridge-Intel(R)
Core(TM) i7-3770 CPU @ 3.40GHz` on platform `Portable Computing Language`, so
these are CPU-device measurements, never GPU parity.

| Check | Result |
|-------|--------|
| `stage-residency-test` (POCL CPU, `RT_REQUIRE_GPU=1`) | PASS, 0 skipped: buffers, upload/readback, reuse, true LRU order, budget clamping, over-budget refusal and host fallback, running/complete/failed events including a driver failure, selection and kill switches, node parity with an explicit executing-device assertion |
| `stage-residency-test` without a device and `RT_REQUIRE_GPU=1` | FAIL, exit 1 (a missing device cannot pass as a skip) |
| `stage-device-test` (POCL CPU) | PASS; a refused compiled host program reports `device=NONE` |
| all 16 stage test executables, CPU and OpenCL trees | PASS; `tools/perf/run-suite.sh` builds 14 of them, including `stage-residency-test` |
| `lanczos3` node parity, synthetic 128x96, scales 0.5 and 1.5 | PAE 0.000000 LSB16, 0 of 9216 and 0 of 82944 samples differ (tolerance 1 LSB16); `StageExecution` reports `device=OPENCL`, `asynchronous=true` |
| over-budget node | runs on the host, `StageExecution` reports `device=CPU`, `asynchronous=false`, exact against the CPU reference |
| 16-bit TIFF export, resize profile, `--gpu=off`, switch off vs on | `AE=0`, PAE 0 of 7604928 samples, same output hash; both runs record the host implementation |
| kill-switch check, `--gpu=off` with `RT_STAGE_OPENCL=1` | the profile records `impl=module` (host), not `impl=opencl`; before the wiring fix it recorded `opencl` |
| `--gpu=auto` with `RT_STAGE_OPENCL=1` | no stage `lanczos3` record: the classic `opencl::resizeLanczos` handles the node, and the export is `AE=0` against `--gpu=off` |
| preview sweep, 16 states, switch off vs on | 32 images `AE=0`, PAE 0 (regression check: the preview graph runs no stage-ABI node and mode 0 disallows the device) |
| crop harness, 3 states plus sequences, switch off vs on | 30 PPM files byte-identical; both runs PASS, 42 plans, 0 fallbacks (no stage-ABI node in the crop graph) |
| preview timing, interleaved 18 samples per side | baseline `9e8df3493` 327.67 ms mean (326.99 median); switch off 331.48 (328.69); switch on 328.99 (324.72); the spread is run-to-run noise |

No performance claim is made for the device node on a CPU OpenCL device; the
device run is slower than the host and exists for parity and correctness.

### Node device coverage

The `Device` column of the canonical block catalogue above is the single source
of truth; this section records the remaining work and the naming exceptions.

Device nodes today: `transform`, `rgb.proc`, `gamut`, `framing`, `lanczos3` (block
`resize`), `output.convert`, `monitor.output`, `viewport.output`,
`thumbnail.output`, `thumbnail.whitebalance`, `thumbnail.scale` and
`thumbnail.transform` (the last
two plus their runtime-only packed siblings `thumbnail.scale.u8` and
`thumbnail.transform.u8`).

Partial: `thumbnail.rgb` is a composite. Its two dominant pixel passes are the
`transform` and `rgb.proc` device nodes, so the block is `partial`; the
colour-space conversion, film negative, first analysis, dehaze, Fattal tone
mapping and local adjustments around them stay on the host. The sub-node
records, the commands and the parity result are in `STAGE_RUNTIME.md`.

Still CPU-only: `source.select`, `source.embedded`, `raw.preprocess`,
`raw.demosaic`, `raw.base`, `analysis`, `local.adjustments`, `lab.tools`,
`lab.sharpening`, `lab.wavelet`, `working.trc`, `ciecam`, `crop`,
`output.sharpen`, `output.icc`, `thumbnail.decode` and `thumbnail.lab`.

Three of these are not standalone pixel kernels and need a recorded exception
rather than a device node: `source.select` and `source.embedded` are I/O, and
`analysis` produces only histograms. `analysis` is also folded into `raw.base`
in the preview path, and the crop path folds `raw.preprocess` and
`raw.demosaic` into `source.select`; those executions have no buffer boundary
to submit.

## Final integration of the request and device branches

The typed-request branch (`perf-graph-request2`, head `81e73b9be`) and the
device-residency branch (`perf-graph-opencl`, head `c42ea4ca4`) were merged in
that order onto `9e8df3493`. The three shared files were combined by hand:

- `rtengine/improccoordinator.cc` keeps the typed planning, the
  `refreshFromLegacyMask()` calls and every `assertPlanningAgrees()` site from
  the request branch, and adds `stage::set_opencl_stage_allowed(useOpenCL &&
  openCLMode == 1)` beside `opencl::setStageAllowed()`; `simpleprocess.cc` sets
  the batch policy from `useOpenCL` alone.
- `rtengine/CMakeLists.txt` keeps the request test sources and
  `RT_SOURCE_DIR`, and adds `stage/stage_opencl_device.cc` and
  `stage-residency-test`.
- This record keeps the request sections and the device section; the earlier
  per-phase test counts are replaced by the integrated counts.

Verification of the merged tree (`build/cpu-perf-graph-final` and
`build/opencl-perf-graph-final`), host CPU build unless stated, image 10.2 MP
PEF `colorspace_flowers.pef`, `OMP_NUM_THREADS=1`:

| Check | Result |
|-------|--------|
| `tools/build-rt.sh cpu -j 6` / `opencl -j 6` | both compile; 16 stage test executables plus `editorrequest-test` PASS in both trees (the two device tests use the pocl-only ICD directory) |
| preview sweep, 16 states, `RT_NODE_CACHE` off | PASS, 32 images (`preview.tif`, `work.tif`) `AE=0` |
| preview sweep, 16 states, `RT_NODE_CACHE=1` | PASS, 32 images `AE=0`; `hits=0 misses=5 refused=11 inserts=5`; cross-cache comparison of the 16 states is `AE=0` |
| repeated exposure, 10 samples, `RT_NODE_CACHE=1` | `hits=7 misses=3 refused=1 inserts=3`; the final dump is `AE=0` for `preview.tif` and `work.tif` against the cache-off run; `stage-node-cache-test` reports `repeated-pass: hits=1 misses=1 AE=0` |
| `crop-stage-test --sequences` | PASS, 33 plans, 0 fallbacks, 33 derivations, `AE=0` |
| `crop-stage-test --stages` | PASS, 6 plans, 0 fallbacks, 6 derivations, `AE=0` |
| `thumbnail-stage-test` | PASS embedded and raw; both PPM pairs byte-identical |
| 16-bit TIFF export vs a `9e8df3493` binary, neutral and sidecar | `AE=0`, 0 of 30419712 samples differ in both |
| `stage-residency-test`, ICD restricted to `pocl.icd`, `RT_OPENCL_ALLOW_CPU=1 RT_REQUIRE_GPU=1` | PASS, 0 skipped; POCL CPU device `cpu-ivybridge-...`; PAE 0.000000 LSB16, 0 of 9216 and 0 of 82944 node samples, 0 of 4608 through `run()`; tolerance 1 LSB16. These are POCL CPU results, not GPU parity |
| same test without a device and `RT_REQUIRE_GPU=1` | FAIL, exit 1, as designed |
| kill switch, `--gpu=off` with `RT_STAGE_OPENCL=1`, resize profile | the stage profile records one `lanczos3` node with `impl=module`; no `impl=opencl` record |
| 16-bit export with the resize profile, switch off (`--gpu=off`) vs on (`--gpu=auto`) | `AE=0`, 0 of 1630800 samples differ; the on run emits no stage `lanczos3` record because the classic path handles it |
| exposure edit timing, node cache off vs on, 10 samples per side | off 333.9 ms mean / 330.4 ms p50 and 318.4 / 317.3 on a repeat; on 309.1 / 302.5 with 7 hits. The off-side spread (16 ms) is the same order as the difference, so no timing claim is made |
| exposure edit timing, device switch off vs on, 10 samples per side | off 319.9 ms mean / 318.7 ms p50, repeat 321.8 / 317.0; on 318.4 / 317.5. Within the host noise range; the preview graph runs no stage-ABI node |
