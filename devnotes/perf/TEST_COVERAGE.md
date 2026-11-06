# Stage test coverage manifest

Traceability record for the canonical stage vocabulary. The ids, work-item
names and path names are the ones defined in
`rtengine/stage/stage_contract.{h,cc}`; `tools/perf/test-coverage.sh` derives
them from that file and fails when a block, work item or path has no row here.

The table answers two questions per block: which host-side test or profile
executes it, and which `tools/opencl/verify.sh` row proves its device path.
`n/a` in the `verify.sh` column means there is no device row, either because
the block has no device node or because the path is not on the export gate; the
reason is named where it is not obvious. A `n/a` there is never evidence of
device parity.

Host coverage comes in three tiers:

- inventory: `stage-pipeline-test` proves the block appears in the canonical
  paths, with the right folds, domains and work items;
- planning: `stage-param-map-test`, `stage-dirty-blocks-test`,
  `stage-work-test` and `stage-node-cache-test` prove the block's parameter
  provenance, invalidation and reuse;
- execution: the path harnesses (`crop-stage-test`, `thumbnail-stage-test`,
  `fast-preview-test`, `working-trc-setup-test`, `test-fast-export-graph.sh`)
  and the `verify.sh` gates run the block on real pixels.

## Block coverage

| Block id | Host test or profile | Paths | verify.sh row |
|---|---|---|---|
| `source.select` | `stage-work-test` (`sourceSelect`); `stage-dirty-blocks-test`; `stage-pipeline-test` | `export`, `fast_export`, `preview`, `crop`, `thumbnail` | n/a (frame selection, no pixel kernel) |
| `source.embedded` | `stage-pipeline-test`; `thumbnail-stage-test` (embedded quick path) | `thumbnail` | n/a (thumbnail path has no export row) |
| `raw.preprocess` | `stage-work-test` (`rawPreprocess`); `stage-param-map-test`; `stage-node-cache-test` | `export`, `fast_export`, `preview`, `crop`, `thumbnail` | `pef/stage-device-raw-preprocess` |
| `raw.demosaic` | `stage-work-test` (`rawDemosaic`); `stage-param-map-test`; `stage-node-cache-test` | `export`, `fast_export`, `preview`, `crop`, `thumbnail` | `pef/stage-device-raw-demosaic`, `pef/stage-device-raw-demosaic-split` |
| `raw.base` | `stage-node-cache-test`; `stage-work-test` (`rawBaseRebuild`); `stage-pipeline-test` | `preview`, `crop`, `thumbnail` | `pef/stage-device-raw-base`, `pef/stage-device-raw-base-wb` |
| `analysis` | `stage-work-test` (`analysisRun`); `stage-dirty-blocks-test` | `export`, `fast_export`, `preview`, `crop` | n/a (analysis has no device node) |
| `transform` | `stage-residency-test`; `crop-stage-test`; `stage-pipeline-test` | `export`, `fast_export`, `preview`, `crop` | `pef/stage-device-transform` |
| `local.adjustments` | `stage-residency-test`; `crop-stage-test --stages`; `fast-preview-test` | `export`, `preview`, `crop` | `pef/stage-device-local-adjustments`, `pef/stage-host-local-adjustments-residual` |
| `rgb.proc` | `stage-residency-test`; `fast-preview-test`; `crop-stage-test` | `export`, `preview`, `crop` | `pef/stage-device-rgbproc` (`--rgbproc` also forces the node) |
| `lab.tools` | `stage-residency-test`; `fast-preview-test`; `crop-stage-test --stages` | `export`, `preview`, `crop` | `pef/stage-device-lab-tools-sh`, `pef/stage-host-lab-tools-residual` |
| `lab.sharpening` | `stage-residency-test`; `crop-stage-test --stages` | `export`, `crop` | `pef/stage-device-lab-sharpening` |
| `lab.wavelet` | `stage-residency-test`; `fast-preview-test`; `crop-stage-test --stages` | `export`, `preview`, `crop` | `pef/stage-device-lab-wavelet` |
| `working.trc` | `working-trc-setup-test`; `stage-residency-test`; `crop-stage-test --stages` | `export`, `preview`, `crop` | `pef/stage-device-working-trc` |
| `ciecam` | `stage-residency-test`; `fast-preview-test`; `crop-stage-test --stages` | `export`, `preview`, `crop` | `pef/stage-device-ciecam` |
| `gamut` | `stage-residency-test`; `fast-preview-test`; `crop-stage-test --stages` | `export`, `preview`, `crop` | `pef/stage-device-gamut` |
| `output.convert` | `stage-pipeline-test`; `stage-param-map-test`; `verify.sh --formats` host export | `export` | `pef/stage-device-output` |
| `crop` | `crop-stage-test`; `stage-pipeline-test`; `stage-param-map-test` | `export` | `pef/stage-device-crop` |
| `resize` | `stage-pipeline-test`; `stage-param-map-test`; `test-export-determinism.sh` | `export` | `pef/stage-device-resize` |
| `output.sharpen` | `stage-residency-test`; `stage-pipeline-test` | `export` | `pef/stage-device-sharpen`, `pef/stage-device-sharpen-rld` |
| `framing` | `stage-pipeline-test`; `stage-param-map-test` | `export` | `pef/stage-device-framing` |
| `output.icc` | `stage-pipeline-test`; `stage-param-map-test` | `export` | n/a (metadata and profile attachment, no pixel kernel) |
| `monitor.output` | `stage-dirty-blocks-test`; `fast-preview-test`; `stage-pipeline-test` | `preview` | `monitor` (`--monitor`, also `run_monitor` in `--quick`) |
| `viewport.output` | `stage-dirty-blocks-test`; `crop-stage-test`; `stage-pipeline-test` | `crop` | n/a (GUI viewport record in `VALIDATION.md`, no export row) |
| `thumbnail.decode` | `stage-pipeline-test`; `thumbnail-stage-test` | `thumbnail` | n/a (thumbnail path has no export row) |
| `thumbnail.scale` | `thumbnail-stage-test`; `stage-residency-test` | `thumbnail` | n/a (thumbnail path has no export row) |
| `thumbnail.transform` | `thumbnail-stage-test`; `stage-residency-test` | `thumbnail` | n/a (thumbnail path has no export row) |
| `thumbnail.whitebalance` | `thumbnail-stage-test`; `stage-residency-test` | `thumbnail` | n/a (thumbnail path has no export row) |
| `thumbnail.rgb` | `thumbnail-stage-test`; `stage-residency-test` | `thumbnail` | n/a (independent reference in `stage-residency-test`; no export row) |
| `thumbnail.lab` | `thumbnail-stage-test`; `stage-residency-test` | `thumbnail` | n/a (independent reference in `stage-residency-test`; no export row) |
| `thumbnail.output` | `thumbnail-stage-test`; `stage-residency-test` | `thumbnail` | n/a (thumbnail path has no export row) |
| `raw.base.source` | `stage-work-test` (`rawBaseRebuild`, `retinexPrepare`, `spotApply`, `spotRestore`); `stage-node-cache-test`; `test-fast-export-graph.sh` | `export`, `fast_export` | `pef/stage-device-raw-base`, `pef/stage-device-raw-base-wb` |
| `raw.denoise` | `stage-work-test` (`denoiseRun`); `stage-pipeline-test` | `export` | n/a (no device node) |
| `raw.color` | `stage-pipeline-test`; `stage-param-map-test`; `test-fast-export-graph.sh` | `export`, `fast_export` | `pef/stage-device-raw-base` (camera-matrix sub-step) |
| `early.resize` | `stage-pipeline-test`; `test-fast-export-graph.sh` | `fast_export` | n/a (fast export has no verify.sh row) |
| `denoise` | `stage-work-test` (`denoiseRun`); `test-fast-export-graph.sh` | `fast_export` | n/a (no device node) |
| `finish` | `stage-pipeline-test`; `test-fast-export-graph.sh` | `fast_export` | n/a (composite finish, no device node) |

## Work item coverage

The names are the diagnostic names returned by `workItemName()`. The paths are
the paths on which the implementing block declares the body; they come from
the `StageWorkItemRule` lists in the contract.

| Work item | Host test or profile | Blocks | Paths |
|---|---|---|---|
| `sourceSelect` | `stage-work-test`; `stage-node-cache-test` | `source.select` | `export`, `fast_export`, `preview`, `crop`, `thumbnail` |
| `rawPreprocess` | `stage-work-test`; `stage-node-cache-test` | `raw.preprocess` | `export`, `fast_export`, `preview` |
| `rawDemosaic` | `stage-work-test`; `stage-node-cache-test` | `raw.demosaic` | `export`, `fast_export`, `preview` |
| `rawBaseRebuild` | `stage-work-test`; `stage-node-cache-test` | `raw.base`, `raw.base.source` | `export`, `fast_export`, `preview`, `crop` |
| `retinexPrepare` | `stage-work-test`; `stage-node-cache-test` | `raw.base`, `raw.base.source` | `export`, `fast_export`, `preview` |
| `spotApply` | `stage-work-test`; `stage-node-cache-test` | `raw.base`, `raw.base.source` | `export`, `fast_export`, `preview`, `crop` |
| `spotRestore` | `stage-work-test` | `raw.base`, `raw.base.source` | `export`, `fast_export`, `preview`, `crop` |
| `analysisRun` | `stage-work-test` | `analysis`, `raw.base` | `export`, `fast_export`, `preview`, `crop` |
| `denoiseRun` | `stage-work-test`; `stage-pipeline-test` | `denoise`, `raw.base`, `raw.denoise` | `export`, `fast_export`, `crop` |

## Path coverage

| Path | Host test or harness | verify.sh row |
|---|---|---|
| `export` | `stage-pipeline-test`; `stage-param-map-test`; `test-export-determinism.sh` | `--quick`, `--formats`, `--stage-device` |
| `fast_export` | `stage-pipeline-test`; `test-fast-export-graph.sh` | n/a (no fast-export row; the harness is the gate) |
| `preview` | `stage-pipeline-test`; `fast-preview-test`; `preview-work-order-sweep`; `test-preview-parity.sh` | `--quick`, `--monitor` |
| `crop` | `stage-pipeline-test`; `crop-stage-test` | `--stage-device` (`pef/stage-device-crop`); GUI check |
| `thumbnail` | `stage-pipeline-test`; `thumbnail-stage-test` | n/a (no export row; the harness is the gate) |

## Adapter registries

Every path adapter owns one registry that maps a contract block id to its
body. The registry is both the coverage list and the dispatch table, so a
missing entry aborts the plan instead of skipping a block.

| Registry | Adapter and location | Coverage |
|---|---|---|
| `StageHandlerRegistry<ExportHandler>` | `ImageProcessor::exportHandlers()` in `rtengine/simpleprocess.cc` | `verify.sh --formats` and `test-export-determinism.sh` dispatch the export nodes; `stage-pipeline-test` proves the inventory |
| `StageHandlerRegistry<ExportHandler>` (fast) | `ImageProcessor::fastExportHandlers()` in `rtengine/simpleprocess.cc` | `test-fast-export-graph.sh` asserts one profile record per fast node |
| `StageHandlerRegistry<PreviewNodeHandler>` | `ImProcCoordinator::previewRegistry()` in `rtengine/improccoordinator.cc` | `fast-preview-test`, `preview-work-order-sweep` and `verify.sh --quick` dispatch the preview nodes |
| `StageHandlerRegistry<CropHandler>` | crop handler table in `rtengine/dcrop.cc` | `crop-stage-test` runs the crop graph for fit, 100%, zoomed and panned viewports and compares the output bytes |
| `StageHandlerRegistry<std::function<bool()>>` | thumbnail tables in `rtengine/rtthumbnail.cc` (`loadQuickFromRaw`, `loadFromRaw`, `quickProcessImage`, `processImage`) | `thumbnail-stage-test` covers the embedded quick path, the raw fallback and the full thumbnail chain |
| `StageHandlerRegistry<Handler>` template | `rtengine/stage/stage_handler.h` | `stage-handler-test` covers ordering, missing handlers, duplicate ids and the null/empty-id aborts in release builds |
| Lanczos loadable-module adapter | `rtengine/stage/adapters/stage_lanczos_module.cc` | `stage-device-test` and `stage-residency-test` exercise the `lanczos3` program; `pef/stage-device-resize` is the end-to-end device row |
