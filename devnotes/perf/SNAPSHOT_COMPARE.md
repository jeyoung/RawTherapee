# Snapshot comparison performance

The feature uses a parameter swap on the single editor coordinator. It does
not keep a second `InitialImage`, preview handler or image area.

Measurement setup: CPU build, GCC 14.2.0, 8 physical cores / 16 threads, Xvfb
1600x1000, 10.2 MP PEF sample, default profile, fit zoom,
`RT_COMPARE_PROFILE=1`.

| Event | Time |
|---|---:|
| Snapshot ready | 364.5 ms |
| Live ready | 346.0 ms |
| Repeat snapshot ready | 355.8–363.7 ms |
| Repeat live ready | 342.8–347.2 ms |

RSS (`VmRSS`, approximate):

| Point | RSS |
|---|---:|
| After image load | 668 MB |
| After first compare and snapshot creation | 731 MB |
| After four compare cycles | 770 MB |

There is no second-source-sized allocation. The extra growth is not fully
attributed; it includes the saved parameter copies, preview-base-cache
activity and allocator growth. The 10 MP fit target of 250 ms was not met; the
measured value is recorded rather than relaxed.

The comparison badge is an overlay, so showing it does not change the viewport
allocation. Frozen-viewport and live-restore checks used screenshot diffs of
the central image area: `AE=0` for drag/wheel during a hold and `AE=0` between
the live view before and after a cycle.
