# Test data

Raw files are never committed to the repository and never uploaded. They live
in `$RT_TESTDATA` (default `~/rt-testdata`).

## Manifest

`testdata.manifest` lists the files used by `tools/opencl/verify.sh`:

- extension
- file name, relative to `$RT_TESTDATA`
- variant description
- SHA-256
- source
- licence

`verify.sh --list-coverage` compares the manifest with
`rawtherapee-cli --list-formats` and reports extensions without a sample.

## Current corpus

| ID | Extension | Variant | Source |
|---|---|---|---|
| RAW-1 | pef | Bayer 14-bit, 10 MP | RawTherapee test image |
| RAW-7 | nef | Bayer 14-bit, 45.7 MP (Nikon D850) | user library |
| IMG-2 | jpg | 8-bit, 45.7 MP | user library |
| IMG-3 | tif | 16-bit RGB scan | user library |
| IMG-4 | tif | 16-bit grey scan | user library |
| IMG-5 | png | 8-bit RGBA | user library |

Local samples are copied into `$RT_TESTDATA/local/`; they are private and
must not be committed or uploaded.

Add rows as files are collected. Rare formats should use CC0 samples from
raw.pixls.us, recorded with their URL, SHA-256 and licence.

## Fetching

Downloaded samples are verified against the manifest:

```bash
tools/opencl/fetch-testdata.sh
```

## Baseline

`colorspace_flowers.pef` is the official RawTherapee sample and the baseline
for `verify.sh --quick`:

- SHA-256 `ddefa07a8b48fd157b48354488b265be48a47c204d00d20d54ead8dfddd9f151`
- source `https://rawtherapee.com/shared/test_images/colorspace_flowers.pef`
