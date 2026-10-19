# Format coverage

Status of every supported input extension. Updated by
`tools/opencl/verify.sh --formats`; do not mark a row PASS without a recorded
sample and a comparison result.

| Extension | Variant | Sample | Load | Export | GPU parity | Notes |
|---|---|---|---|---|---|---|
| pef | Bayer 14-bit | colorspace_flowers.pef | pending | pending | pending | baseline |

## Variants still to collect

- PNG: 8-bit, 16-bit, grayscale, alpha, embedded ICC, palette (expected
  rejection), interlaced (expected rejection)
- JPEG: 8-bit, grayscale, CMYK, embedded ICC, EXIF orientation
- TIFF: 8-bit, 16-bit, 32-bit float, LogLuv, compressed/uncompressed,
  embedded ICC, EXIF orientation
- JXL: 8/16-bit, float, embedded ICC
- RAW: Bayer, X-Trans, CR3, DNG, Foveon, monochrome, ≥ 40 MP

## Output formats

JPEG 8-bit; TIFF 8/16/16f/32f, compressed and uncompressed, BigTIFF; PNG 8/16.
Each must decode, report the expected bit depth and dimensions, preserve
metadata and match the CPU result within one 16-bit LSB.
