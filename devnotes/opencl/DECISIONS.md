# OpenCL decisions

Short architecture decision records. Append new decisions; do not rewrite
history.

## D001 — Single binary with a runtime toggle

Date: 2026-10-19

Decision: ship one binary built with `WITH_OPENCL=ON`; the user enables or
disables acceleration in Preferences. `WITH_OPENCL=OFF` remains available for
baseline builds and CI.

Alternatives: a second `rawtherapee-opencl` executable, or two packages. Both
add packaging and maintenance cost without adding capability.

## D002 — Runtime loading, no link dependency

Date: 2026-10-19

Decision: open the ICD loader with `Glib::Module`, resolve entry points, and
fall back to the CPU when anything is missing.

Consequences: no `libOpenCL` dependency in packages; the application starts on
systems without OpenCL; a missing device is a normal condition, not an error.

## D003 — OpenCL C 1.2

Date: 2026-10-19

Decision: kernels target OpenCL 1.2. Avoid vendor extensions and 2.0+ features.

Consequences: broad device coverage, including Mesa Clover/Rusticl, POCL and
older NVIDIA drivers. Some optimisations (subgroups, fine-grain SVM) are not
available.

## D004 — Stateless stage adapters in v1

Date: 2026-10-19

Decision: each accelerated stage uploads its inputs, runs, and downloads its
output. No device-resident pipeline in v1.

Consequences: simple correctness and fallback semantics. Transfer overhead is
accepted for the first stages because they are compute-heavy. Residency is a
later optimisation, only if profiling justifies it.

## D005 — CPU build must remain byte-identical

Date: 2026-10-19

Decision: the CPU path and `--gpu=off` must produce byte-identical output to a
`WITH_OPENCL=OFF` build of the same commit.

Consequences: no refactoring of CPU algorithms while porting them; GPU paths
are additive and gated by tests.
