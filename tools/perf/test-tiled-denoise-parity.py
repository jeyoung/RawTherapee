#!/usr/bin/env python3
"""Compare tiled denoise across two installed CLIs on deterministic TIFF inputs.

Requires NumPy and ImageMagick's magick command for tools/opencl/compare_tiff.py.
Uses one OpenMP thread throughout; denoise limits 1 and 4 both force tiling.
Scratch must be new or empty and is preserved on success and failure.
"""

import argparse
import array
import hashlib
import json
import math
import os
from pathlib import Path
import re
import shutil
import struct
import subprocess
import sys

FIXTURES = {"one-row": (4096, 512), "one-column": (512, 2304), "odd-band": (1024, 1793)}
PROFILE = """[Version]
Version=352
[Exposure]
Auto=false
HistogramMatching=false
[Directional Pyramid Denoising]
Enabled=true
Enhance=false
Median=false
Luma=30
Ldetail=25
Chroma=30
Method=Lab
LMethod=SLI
CMethod=MAN
C2Method=AUTO
Gamma=1.7
Passes=1
LCurve=0;
CCCurve=0;
[Resize]
Enabled=false
"""


def make_environment(settings, cache, tile_order):
    env = {k: v for k, v in os.environ.items() if not k.startswith(("RT_", "OMP_"))}
    env.update(
        OMP_NUM_THREADS="1",
        OMP_THREAD_LIMIT="1",
        OMP_MAX_ACTIVE_LEVELS="1",
        OMP_DYNAMIC="FALSE",
        RT_SETTINGS=str(settings),
        RT_CACHE=str(cache),
        RT_TILE_ORDER=str(tile_order),
        RT_STAGE_OPENCL="0",
        TMPDIR=str(settings.parent / "tmp"),
    )
    return env


def sha256(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def write_fixture(path, width, height):
    """Write one-strip uncompressed RGB16, using only the standard library."""
    aux = 8 + 2 + 12 * 12 + 4
    entries = [
        (256, 4, 1, width),
        (257, 4, 1, height),
        (258, 3, 3, aux),
        (259, 3, 1, 1),
        (262, 3, 1, 2),
        (273, 4, 1, aux + 12),
        (277, 3, 1, 3),
        (278, 4, 1, height),
        (279, 4, 1, width * height * 6),
        (284, 3, 1, 1),
        (317, 3, 1, 1),
        (339, 3, 3, aux + 6),
    ]
    header = b"II" + struct.pack("<HIH", 42, 8, len(entries))
    for tag, kind, count, value in entries:
        header += struct.pack("<HHI", tag, kind, count)
        header += (
            struct.pack("<H", value) + b"\0\0"
            if kind == 3 and count == 1
            else struct.pack("<I", value)
        )
    header += struct.pack("<I6H", 0, 16, 16, 16, 1, 1, 1)
    pixels = array.array("H")
    state = 0x13579BDF
    for y in range(height):
        for x in range(width):
            base = (
                16000 + 30000 * x // max(1, width - 1) + 4000 * y // max(1, height - 1)
            )
            for channel in range(3):
                state = (1664525 * state + 1013904223) & 0xFFFFFFFF
                noise = ((state >> 16) & 0xFFFF) - 32768
                pixels.append(
                    max(0, min(65535, base + noise // 3 + (channel - 1) * 1500))
                )
    if sys.byteorder != "little":
        pixels.byteswap()
    with path.open("wb") as stream:
        stream.write(header)
        pixels.tofile(stream)


def read_tiff_geometry(path):
    data = path.read_bytes()
    endian = "<" if data[:2] == b"II" else ">" if data[:2] == b"MM" else None
    if endian is None or struct.unpack_from(endian + "H", data, 2)[0] != 42:
        raise ValueError("output is not a classic TIFF")
    offset = struct.unpack_from(endian + "I", data, 4)[0]
    count = struct.unpack_from(endian + "H", data, offset)[0]
    if count > 1024:
        raise ValueError("invalid TIFF directory size")
    tags = {}
    for index in range(count):
        pos = offset + 2 + index * 12
        tag, kind, number = struct.unpack_from(endian + "HHI", data, pos)
        if kind not in (3, 4) or number > 1024:
            continue
        size, code = (2, "H") if kind == 3 else (4, "I")
        location = (
            pos + 8
            if size * number <= 4
            else struct.unpack_from(endian + "I", data, pos + 8)[0]
        )
        tags[tag] = struct.unpack_from(endian + str(number) + code, data, location)
    required = {256, 257, 258, 262, 277}
    if not required.issubset(tags):
        raise ValueError("missing RGB TIFF geometry/format tags")
    expected = {
        258: (16, 16, 16),
        259: (1,),
        262: (2,),
        277: (3,),
        284: (1,),
        317: (1,),
    }
    if any(tags.get(tag, value) != value for tag, value in expected.items()):
        raise ValueError("output must be uncompressed chunky RGB16")
    if 258 not in tags or 277 not in tags or any(v != 1 for v in tags.get(339, (1,))):
        raise ValueError("output must contain unsigned RGB16 samples")
    width, height = tags[256][0], tags[257][0]
    if width <= 0 or height <= 0:
        raise ValueError("invalid TIFF geometry")
    return width, height


def parse_comparison(stdout, returncode, expected_total):
    """Validate the comparator protocol; its exit 0 alone permits one LSB."""
    if returncode not in (0, 1):
        raise ValueError("TIFF comparator failed")
    fields = stdout.split()
    if len(fields) != 4:
        raise ValueError("invalid TIFF comparator metrics")
    maximum, normalized, differing, total = (
        int(fields[0]),
        float(fields[1]),
        int(fields[2]),
        int(fields[3]),
    )
    consistent = (
        math.isfinite(normalized)
        and 0 <= maximum <= 65535
        and 0 <= differing <= total
        and total == expected_total
        and total > 0
        and (maximum == 0) == (differing == 0)
        and math.isclose(normalized, maximum / 65535, rel_tol=1e-7, abs_tol=1e-12)
        and returncode == (0 if maximum <= 1 else 1)
    )
    if not consistent:
        raise ValueError("inconsistent TIFF comparator metrics")
    return dict(
        max_abs=maximum,
        normalized_max=normalized,
        differing=differing,
        total=total,
        exit=returncode,
    )


def compare_exact(metrics):
    return (
        metrics["exit"] == 0
        and metrics["max_abs"] == 0
        and metrics["normalized_max"] == 0
        and metrics["differing"] == 0
    )


def save_report(scratch, report):
    (scratch / "report.json").write_text(json.dumps(report, indent=2) + "\n")


def preflight(args, comparator):
    for binary in (args.base_cli, args.candidate_cli):
        if not binary.is_file() or not os.access(binary, os.X_OK):
            raise ValueError("both CLI paths must be executable files")
    if not comparator.is_file() or shutil.which("magick") is None:
        raise ValueError("requires compare_tiff.py and ImageMagick magick")
    subprocess.run(
        [sys.executable, "-c", "import numpy"],
        check=True,
        capture_output=True,
        timeout=args.timeout,
    )


def run_comparison(comparator, left, right, total, log, timeout):
    temporary = log.parent / "tmp"
    temporary.mkdir(exist_ok=True)
    env = os.environ.copy()
    env.update(TMPDIR=str(temporary), MAGICK_TEMPORARY_PATH=str(temporary))
    result = subprocess.run(
        [sys.executable, str(comparator), str(left), str(right), "--bits", "16"],
        env=env,
        capture_output=True,
        text=True,
        timeout=timeout,
    )
    log.write_text(result.stdout + result.stderr)
    return parse_comparison(result.stdout, result.returncode, total)


def validate_tile_markers(markers, fixture, role):
    # Older baselines may predate the merge diagnostic; candidates must prove
    # tiled execution rather than only reporting active denoise threads.
    if role == "candidate" and not markers:
        raise ValueError("candidate is missing the tiled-denoise diagnostic")
    for marker in markers:
        rows, cols, buffers, width, height = map(int, marker)
        valid = min(rows, cols, buffers, width, height) > 0 and rows * cols > 1
        if fixture == "one-row":
            valid = valid and rows == 1 and cols > 1
        elif fixture == "one-column":
            valid = valid and rows > 1 and cols == 1
        elif fixture == "odd-band":
            valid = valid and rows > 1 and rows % 2 == 1 and cols > 1
        if not valid:
            raise ValueError(f"{role}/{fixture}: unexpected denoise tile layout")


def export(
    args, scratch, role, binary, fixture, shape, limit, order, disabled, repeat=False
):
    name = f"{fixture}-disabled" if disabled else f"{fixture}-limit{limit}-order{order}"
    relative = Path(role + "-repeat" if repeat else role) / name
    folder = scratch / relative
    folder.mkdir(parents=True)
    settings, cache = folder / "settings", folder / "cache"
    settings.mkdir()
    cache.mkdir()
    (folder / "tmp").mkdir()
    options = f"[General]\nVerbose=true\n[Performance]\nRgbDenoiseThreadLimit={limit}\n"
    (settings / "options").write_text(options)
    profile = "disabled.pp3" if disabled else "active.pp3"
    command = [
        str(binary),
        "--gpu=off",
        "-p",
        str(scratch / profile),
        "-o",
        str(folder / "output.tif"),
        "-t",
        "-b16",
        "-Y",
        "-c",
        str(scratch / "fixtures" / (fixture + ".tif")),
    ]
    with (folder / "cli.log").open("w") as log:
        process = subprocess.run(
            command,
            env=make_environment(settings, cache, order),
            stdout=log,
            stderr=subprocess.STDOUT,
            timeout=args.timeout,
        )
    if process.returncode != 0:
        raise ValueError(f"{role}/{name}: CLI failed")
    if read_tiff_geometry(folder / "output.tif") != shape:
        raise ValueError(f"{role}/{name}: output geometry mismatch")
    body = (folder / "cli.log").read_text(errors="replace")
    threads = re.findall(
        r"RGB_denoise uses (\d+) main thread\(s\) and up to (\d+) nested", body
    )
    if not disabled and (
        not threads
        or any(int(main) != 1 or int(nested) != 1 for main, nested in threads)
    ):
        raise ValueError(
            f"{role}/{name}: missing or unexpected denoise thread diagnostic"
        )
    if "insufficient memory" in body or "Output is not denoised" in body:
        raise ValueError(f"{role}/{name}: denoise allocation retry/failure")
    markers = re.findall(
        r"RGB_denoise merge buffers: rows=(\d+) cols=(\d+) buffers=(\d+) tile=(\d+)x(\d+)",
        body,
    )
    if not disabled:
        validate_tile_markers(markers, fixture, role)
    sanitized = [
        value.replace(str(binary), f"${role.upper()}_CLI").replace(
            str(scratch), "$SCRATCH"
        )
        for value in command
    ]
    return dict(
        case=str(relative),
        command=sanitized,
        exit=process.returncode,
        output=str(relative / "output.tif"),
        output_file_sha256=sha256(folder / "output.tif"),
        log=str(relative / "cli.log"),
        settings_sha256=sha256(settings / "options"),
        thread_reports=threads,
        allocation_reports=markers,
    )


def compare_outputs(args, scratch, comparator, report):
    for name, (width, height) in FIXTURES.items():
        for limit in (1, 4):
            for order in (0, 1):
                case = f"{name}-limit{limit}-order{order}"
                metrics = run_comparison(
                    comparator,
                    scratch / "base" / case / "output.tif",
                    scratch / "candidate" / case / "output.tif",
                    width * height * 3,
                    scratch / "candidate" / case / "parity-comparison.log",
                    args.timeout,
                )
                report["comparisons"].append(dict(case=case, **metrics))
                save_report(scratch, report)
                if not compare_exact(metrics):
                    raise ValueError(f"{case}: before/after raster samples differ")


def run_harness(args):
    scratch = args.scratch.resolve()
    if scratch.exists() and (not scratch.is_dir() or any(scratch.iterdir())):
        raise ValueError("scratch must be a new or empty directory")
    scratch.mkdir(parents=True, exist_ok=True)
    report = dict(
        status="FAIL",
        omp_threads=1,
        denoise_limits=[1, 4],
        tile_orders=[0, 1],
        exports=[],
        comparisons=[],
        determinism_comparisons=[],
    )
    comparator = Path(__file__).resolve().parents[1] / "opencl/compare_tiff.py"
    try:
        preflight(args, comparator)
        report["binaries"] = {
            role: dict(sha256=sha256(binary))
            for role, binary in (
                ("base", args.base_cli),
                ("candidate", args.candidate_cli),
            )
        }
        (scratch / "fixtures").mkdir()
        for name, shape in FIXTURES.items():
            write_fixture(scratch / "fixtures" / (name + ".tif"), *shape)
        report["fixtures"] = {
            name: dict(
                width=w, height=h, sha256=sha256(scratch / "fixtures" / (name + ".tif"))
            )
            for name, (w, h) in FIXTURES.items()
        }
        for name, body in (
            ("active", PROFILE),
            ("disabled", PROFILE.replace("Enabled=true", "Enabled=false")),
        ):
            (scratch / (name + ".pp3")).write_text(body)
        report["profiles"] = {
            name: sha256(scratch / (name + ".pp3")) for name in ("active", "disabled")
        }
        for role, binary in (
            ("base", args.base_cli),
            ("candidate", args.candidate_cli),
        ):
            for name, shape in FIXTURES.items():
                control = export(args, scratch, role, binary, name, shape, 1, 0, True)
                report["exports"].append(control)
                for limit in (1, 4):
                    for order in (0, 1):
                        active = export(
                            args,
                            scratch,
                            role,
                            binary,
                            name,
                            shape,
                            limit,
                            order,
                            False,
                        )
                        report["exports"].append(active)
                        metrics = run_comparison(
                            comparator,
                            scratch / control["output"],
                            scratch / active["output"],
                            shape[0] * shape[1] * 3,
                            scratch / Path(active["case"]) / "control-comparison.log",
                            args.timeout,
                        )
                        active["versus_disabled"] = metrics
                        save_report(scratch, report)
                        if metrics["differing"] == 0:
                            raise ValueError(
                                f"{active['case']}: denoise did not change disabled control"
                            )
                        repeated = export(
                            args,
                            scratch,
                            role,
                            binary,
                            name,
                            shape,
                            limit,
                            order,
                            False,
                            repeat=True,
                        )
                        report["exports"].append(repeated)
                        save_report(scratch, report)
                        if repeated["settings_sha256"] != active["settings_sha256"]:
                            raise ValueError(
                                f"{active['case']}: repeat settings differ"
                            )
                        repeat_metrics = run_comparison(
                            comparator,
                            scratch / active["output"],
                            scratch / repeated["output"],
                            shape[0] * shape[1] * 3,
                            scratch
                            / Path(repeated["case"])
                            / "determinism-comparison.log",
                            args.timeout,
                        )
                        report["determinism_comparisons"].append(
                            dict(
                                role=role,
                                case=active["case"],
                                original_output=active["output"],
                                repeat_output=repeated["output"],
                                **repeat_metrics,
                            )
                        )
                        save_report(scratch, report)
                        if not compare_exact(repeat_metrics):
                            raise ValueError(
                                f"{active['case']}: same-binary determinism failure"
                            )
        compare_outputs(args, scratch, comparator, report)
        report["status"] = "PASS"
    except (OSError, ValueError, struct.error, subprocess.SubprocessError) as error:
        message = str(error)
        for source, replacement in (
            (scratch, "$SCRATCH"),
            (args.base_cli, "$BASE_CLI"),
            (args.candidate_cli, "$CANDIDATE_CLI"),
            (comparator, "$COMPARATOR"),
            (sys.executable, "$PYTHON"),
        ):
            message = message.replace(str(source), replacement)
        report["error"] = message
    save_report(scratch, report)
    print(
        f"{report['status']}: {len(report['exports'])} exports; {len(report['comparisons'])} parity comparisons"
    )
    return 0 if report["status"] == "PASS" else 1


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--base-cli", type=Path, required=True)
    parser.add_argument("--candidate-cli", type=Path, required=True)
    parser.add_argument("--scratch", type=Path, required=True)
    parser.add_argument(
        "--timeout", type=int, default=180, help="timeout per subprocess in seconds"
    )
    args = parser.parse_args(argv)
    if args.timeout <= 0:
        parser.error("timeout must be positive")
    args.base_cli, args.candidate_cli = (
        args.base_cli.resolve(),
        args.candidate_cli.resolve(),
    )
    try:
        return run_harness(args)
    except (OSError, ValueError) as error:
        print(f"FAIL: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
