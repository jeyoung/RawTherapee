# Reviews

## R1 — M0 to M2

Independent read-only review of `b2ee504c4..HEAD`.

Findings and resolution:

| Severity | Finding | Resolution |
|---|---|---|
| High | Lazy init unsynchronised; concurrent first use silently fell back | Recursive backend mutex; init/available/devices/shutdown/reconfigure serialised; state atomic |
| High | Shutdown could free objects in use | Shutdown and reconfigure take the same lock as stages; RAII helpers stay inside the lock |
| High | Vendor filter could hide a working POCL behind a stale GPU ICD | ICD library must now be loadable; filter directory is content-hashed; absolute cache dir required; opt-out documented |
| Medium | Preferences changes needed a restart | `reconfigure()` called after OK and after reset to defaults |
| Medium | `verify.sh` reported PASS with no device | SKIP is reported unless `RT_ALLOW_CPU_PARITY=1` |
| Medium | Imagefloat resize had no automated coverage | Manual harness passed at 0.1/0.37/0.5/1.8; automated coverage still outstanding |
| Medium | No total device memory budget | 80% of global memory checked before allocation |
| Medium | Windows cache replace failed | Target removed before retry; temp names now unique |
| Medium | Cache payload not integrity checked | Magic and version checked; driver rejects the rest and the file is rebuilt |
| Low | Row pointers formed before validation | Dimension and stride checks added |
| Low | Missing `CL_API_CALL` | Added to all function pointers |

## R2 — review of the fixes

Second independent review of the fix diff (commit `c652e3505`).

| Finding | Resolution |
|---|---|
| `--formats` never detected a device | `detect_device` added; whole run reports SKIP without a device |
| Vendor filter still hides POCL if the GPU runtime has no device | Documented; select POCL with `OCL_ICD_VENDORS`. A probing implementation is outstanding |
| ICD paths hardcoded for x86-64 | Replaced with a `Glib::Module` loadability probe |
| Reset to defaults clobbered `cacheDir` | Preserved around the copy, then reconfigure |
| Settings copy races with workers | Stage gating now uses an atomic backend flag and a cache-directory snapshot; the broader pre-existing `Options` copy race remains |

## R3 — M3 transform

Independent review of the transform stage.

| Finding | Resolution |
|---|---|
| Wrong centre for cropped destinations (`dst` vs `oW/oH`) | `transformRotateScale` now takes `oW/oH` and uses them for the centre |
| Log temp allocated before GPU availability was known | Hook now gates on `opencl::enabled()` before allocating the log copy |
| Vertical cubic sum order differed from CPU SIMD | Changed to `(v0+v1)+(v2+v3)` for vertical and `(v0+v2)+(v1+v3)` for horizontal |
| Log/bilinear paths unexercised | Added `rotate-standard.pp3` and `rotate-scale.pp3`; GPU log path tested on PEF and 45 MP NEF |
| Manifest policy contradicted private rows | Private rows removed from the public manifest and marked uncovered |
| `DeviceInfo::fp64` unused | Removed |

## R4 — M4 capture sharpening

Independent review found that the first kernel draft did not compile: the
parameter name `kernel` is a reserved OpenCL keyword, so every call silently
fell back and the initial PAE 0 was a CPU-vs-CPU artefact.

| Finding | Resolution |
|---|---|
| `kernel` parameter name | Renamed to `weights`; stage kernels are exercised by `--stage capture` with a run assertion |
| Uninitialised `tmp` halo | Local `tmp` is filled with 1.0f before iterations |
| Iterations only updated the 32x32 centre | Div/mult now iterates the full `[radius, full-radius)` tile, matching CPU |
| Overlapping end tiles raced | Four ordered phases: normal, right edge, bottom edge, corner |
| No local-memory/workgroup check | `DeviceInfo` records local memory and max workgroup; stage falls back below 35 KiB/1024 |
| Preview used GPU in export-only mode | Thread-local stage permission set by SimpleProcess/ImProcCoordinator |
| Compile after costly uploads | Program is built before staging the planes; timer starts after success |
| 45 MP watchdog reset | GPU capture is limited to 12 MP; larger images use CPU |

## R5 — M5 buffer pool

The pool review found that cached VRAM was invisible to the stage budget and
the pool could grow without eviction.

| Finding | Resolution |
|---|---|
| Pool not counted in stage memory budget | `ensureBudget()` clears the pool when a stage would exceed 80% of device memory |
| Pool grow-only and exact-size only | Per-device cap (512 MiB or 1/8 VRAM) and eviction on allocation failure |
| `emplace_back` in a destructor could throw | Release the buffer instead of growing the pool |
| `BufferSet` implicitly copyable | Copy and assignment deleted |

## R6 — M6 portable package

The review found missing GTK caches, stale stripping paths, hardcoded identity,
and an icon collision.

| Finding | Resolution |
|---|---|
| Loader/IME caches absent | Absolute query tools, caches generated and rewritten to `/usr/local/lib/rawtherapee` |
| Hardcoded version/prefix | Version derived from git; package version includes commit count/hash; prefix `/usr/local` per user request |
| `strip` path wrong | Strips the real binaries under `/usr/local/lib/rawtherapee` and recomputes size |
| Icon collision | Unique `rawtherapee-5.13` icon and desktop file |
| Lensfun data missing | Bundled under `/usr/local/share/lensfun` and compiled path set |
| Tarball xz missing on minimal systems | Switched to gzip |
| `--image` ignored | Dockerfile takes `BASE_IMAGE`; builds use `--platform linux/amd64` |
| Wrapper relocation | Wrappers derive `/usr/local` from their own location |

## R7 — Debian 12+ package

The self-contained GTK bundle crashed on Debian 13 in `Gtk::Menu::attach`
after SVG icon lookups returned null. The requirement is now one package for
Debian 12 and newer, so the GTK bundle was replaced with host GTK and only
soname-incompatible libraries are bundled.

| Finding | Resolution |
|---|---|
| Bundled GTK/GLib mixed with host desktop stack, GUI segfault | Host GTK/gtkmm; bundle only exiv2 0.27 and JPEG XL 0.7 |
| `debian/` shlibdeps scratch files installed at `/` | Removed before `dpkg-deb` |
| Bundled JPEG XL needs `libbrotli1` | Added to `Depends` |
| Lensfun compiled path pointed at an empty `/usr/local/share/lensfun` | Left empty so lensfun uses its system search path; data is recommended |
| Stale tarball bundle could shadow host GTK | Documented removal before install; old same-name packages are replaced by dpkg |
| Hardcoded package identity | Package name/version derived from the latest git tag and HEAD |
| `--check-deps` checked the host for portable builds | Checks Docker instead |

## R8 — Rusticl auto-detection

The review found an inverted GLib return check, a missing CPU-build stub and
hybrid-GPU handling that enabled only one driver.

| Finding | Resolution |
|---|---|
| `g_setenv()` success test inverted | Uses the GLib `TRUE` result; explicit preference changes now update the injected value |
| CPU-only Linux build missed `autoRusticlDriver()` | Stub returns an empty driver |
| AMD + Intel systems enabled only `radeonsi` | Detection joins both as `radeonsi,iris` |
| Hardcoded English placeholder | Uses the translated `PREFERENCES_OPENCL_RUSTICL_AUTO` string |
| `mesa-opencl-icd` listed twice | Removed from `Suggests`; remains in `Recommends` |
| Uninitialised preference entry pointer | Initialised to `nullptr` |
| Recommending Mesa could hide POCL on CPU-only hosts | POCL is filtered only when an AMD, Intel or NVIDIA DRM card is present |
| Empty external `RUSTICL_ENABLE` disabled auto-detection | Empty is treated as unset |

## Outstanding

- Automated Imagefloat resize coverage (the manual harness was not committed).
- Device probing before vendor filtering.
- Buffer pool to avoid per-call allocation churn.
- End-to-end speedup requires more stages or pipeline residency.

## R9 — Tiled capture sharpening

The first tiled version removed the pixel cap but still lost the context when
large chunks were queued without completion points.

| Finding | Resolution |
|---|---|
| Fixed 16-row chunks do not bound work for radius/iteration extremes | Chunk rows derive from a work budget; settings above the budget fall back to CPU |
| Queue accumulated chunks without completion | `clFinish` after every chunk, preserving phase order and bounding watchdog exposure |
| Host staging allocation could throw past the fallback | Allocations are guarded and return `false` |
| A lost device left later stages retrying a dead session | Fatal enqueue/read failures disable the stage for the remainder of the run |
| POCL was selected like a GPU in auto mode | CPU OpenCL devices require explicit selection or `RT_OPENCL_ALLOW_CPU=1`; native CPU is used otherwise |
| Empty no-op edge chunks | Phases 2/3 start at the final chunk |
| Chunk estimate ignored the two convolution passes and output pixels | Work estimate includes both passes and a 1.4 output-pixel factor; budget is empirical and conservative |
| POCL-only GUI could not select POCL explicitly | Device availability and auto-selection are separate; the device combo remains enabled |
| `RT_OPENCL_ALLOW_CPU=0` enabled POCL | Only the exact value `1` opts in |
| Self-test fell back to device 0 with no selected device | Self-test now reports no GPU device |

Historical note: the R4 capture entry that said “45 MP watchdog reset — GPU
capture limited to 12 MP” is superseded by this work.

## R10 — Preview latest-wins

A mid-render cancellation prototype was rejected. It required requeueing
partial state and raced with preview stop/teardown.

| Finding | Resolution |
|---|---|
| Mid-render abort leaked or partially updated preview state | Backed out; cancellation now happens only before a render starts |
| Generation was not bumped on normal edits | Both `startProcessing(int)` and `endUpdateParams(int)` bump under the params mutex |
| Panning output could be lost on requeue | Requeue dirties the monitor-output block when panning-related |
| Crop transform augmentation was skipped after requeue | Applied whenever the crop block is dirty, including combined requests |
| Changes that dirtied no block caused churn | Only requests that dirty a block bump the generation |

The remaining work is a stage cache and a scripted GUI latency measurement.

## R11 — Preview base cache

The first preview cache was rejected before commit. It saved the image after
colour conversion and was unreachable for ordinary edits.

| Finding | Resolution |
|---|---|
| Base stage only runs for INIT/DENOISE/HDR | No cache committed; next cache must target the Lab/colour boundary |
| Saved image was post-conversion | Avoided; a future cache must save the exact stage input |
| Cache overhead even when disabled | No cache shipped |
| Timing included lock wait and dropped requests | Timer starts after the stale-request check |

## R12 — sRGB output conversion

The first draft used the generic sRGB matrix and gamma LUT from
`iccmatrices.h` and `Color::gammatab_srgb`. Eight-bit spot checks looked fine,
but encoded 16-bit comparison showed a 26 LSB difference from lcms.

| Finding | Resolution |
|---|---|
| Generic sRGB constants differ from the shipped profile | Extract the actual profile matrix and inverse curve at runtime; PAE drops to 1 LSB |
| Profile values were duplicated in the binary | Extracted from `RTv4_sRGB.icc` via `buildGpuProfile()`; no colour values are compiled in |
| Gate included legacy and user-provided names | Limit to the shipped `RTv4_sRGB` profile |
| `NoICMString` was offloaded | Kept on the manual CPU path, which is faster than an upload/download round trip |
| 32-bit float output would be clipped | CLI records `tiffFloat`; float output keeps lcms |
| Thread-local stage gate was not checked | Added `threadStageAllowed` to the stage precondition |
| `compare_tiff.py` linearised samples through ImageMagick | Normalise to uncompressed 16-bit TIFF and read encoded samples |
| LUT and matrix rebuilt on every call | Accepted for now; 256 KiB upload is small relative to the stage |

## R13 — Monitor Lab output

The first monitor implementation used the generic sRGB matrix and was slower
than the multi-threaded CPU manual path. It was replaced with a profile-aware
matrix/TRC path, which only pays off when it replaces lcms.

| Finding | Resolution |
|---|---|
| Generic sRGB path was 2 bytes from lcms and slower than the manual CPU path | Use the monitor profile matrix and inverse TRC; keep the manual CPU path when there is no monitor profile |
| Bisection inversion cost 50–83 ms per profile update | Use it only for the 16-bit output path and cache the result; the monitor path reuses the cache while its profile key is unchanged |
| `cmsIsMatrixShaper()` does not rule out LUT output tags | Reject profiles with `BToA*` or `BToD*` tags |
| Sampled TRC checks could miss non-monotonic or unequal curves | Use `cmsIsToneCurveMonotonic()` and compare the full estimated tables |
| BPC filename heuristic was fragile | Use `cmsDetectDestinationBlackPoint()` and require a zero black point when BPC is on |
| GPU could run when lcms transform creation failed | Require `monitorTransform != nullptr` before setting the GPU-ready flag |
| No minimum-size gate | Remaining work; small detail crops may still favour lcms |
| No committed monitor regression test | `--gpu-test-monitor` and `verify.sh --monitor` compare CPU lcms with GPU on deterministic full-range Lab |
| GPU-ready fields are unsynchronised | Current coordinator/crop call graph serialises them; document if a future caller changes ownership |

## R14 — Opt-in GPU rgbProc

The first fast path allowed `ClampOOG=false` and its verification could pass
without running the stage.

| Finding | Resolution |
|---|---|
| Tone curve group semantics differ when `ClampOOG=false` | Require `ClampOOG=true` for the fast path |
| `verify.sh --rgbproc` did not assert the stage ran | Set `expect_stage=1` and require `opencl rgbproc:`; add an exposure profile |
| Most kernel branches lacked committed tests | Neutral and exposure/highlight/black profiles are committed; mixer/RGB-curve coverage remains outstanding |
| Preview never uses the stage | Documented as batch-only because of the tone-curve histogram |
| A stage failure disabled all OpenCL stages | Latch only `rgbProcUnavailable`; other stages continue |
| Stage-local unavailability was invisible to the caller | Add `rgbProcAvailable()` to the predicate |
| Budget omitted the identity LUT | Count nine LUT buffers |
| Missing shadow LUT silently became identity | Reject `toneCurveBlack != 0` without a shadow LUT |
| LUT copies and uploads happen per call | Accepted for the opt-in stage; cache later if fusion lands |

## R15 — Runtime ICC extraction

The output path originally duplicated the shipped sRGB matrix and parametric
curve. That made a profile update or a same-name custom profile use stale
colour data. The monitor path already read profile tags, but used a 16-bit
estimated table that was not precise enough for 16-bit output.

| Finding | Resolution |
|---|---|
| Output matrix and TRC were hardcoded | Extract matrix and inverse TRC from the actual profile via `buildGpuProfile()` |
| Estimated-table inversion is too coarse for 16-bit output | Invert the monotonic profile curve numerically with 20 bisection steps and cache the table |
| A calibrated monitor profile may have per-channel TRCs or LUT tags | Require identical monotonic TRCs and reject LUT tags; fall back to lcms |
| BPC was assumed safe | Call `cmsDetectDestinationBlackPoint()` and require a zero black point when BPC is enabled |
| Monitor BPC failure could retain a stale ready flag | Clear `monitorGpuReady` on every unsafe branch |
| A silent output fallback would still pass parity | `verify.sh --quick` now asserts that `opencl lab2srgb:` runs |
| Profile setup cost is paid per image | Accepted at about 65 ms; the output job caches it by profile handle. A process-wide cache remains future work |
