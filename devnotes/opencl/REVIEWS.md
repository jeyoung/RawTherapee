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

## Outstanding

- Automated Imagefloat resize coverage (the manual harness was not committed).
- Device probing before vendor filtering.
- Buffer pool to avoid per-call allocation churn.
- End-to-end speedup requires more stages or pipeline residency.
