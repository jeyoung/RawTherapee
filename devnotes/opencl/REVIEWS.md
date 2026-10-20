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
| Manifest policy contradicted private rows | Header now distinguishes public and local private rows |
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

## Outstanding

- Automated Imagefloat resize coverage (the manual harness was not committed).
- Device probing before vendor filtering.
- Buffer pool to avoid per-call allocation churn.
- End-to-end speedup requires more stages or pipeline residency.
