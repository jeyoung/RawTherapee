"""Regression tests for the tiled-denoise parity harness."""

import importlib.util
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location(
    "denoise_parity", ROOT / "test-tiled-denoise-parity.py"
)
HARNESS = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(HARNESS)
COMPARATOR = ROOT.parent / "opencl/compare_tiff.py"
DEPENDENCIES = shutil.which("magick") and importlib.util.find_spec("numpy")

FAKE_CLI = """#!/usr/bin/env python3
import os
from pathlib import Path
import struct
import sys
args = sys.argv[1:]
assert os.environ["OMP_NUM_THREADS"] == "1"
assert os.environ["OMP_THREAD_LIMIT"] == "1"
assert os.environ["RT_STAGE_OPENCL"] == "0"
assert all(flag in args for flag in ["--gpu=off", "-t", "-b16", "-Y"])
source = Path(args[args.index("-c") + 1])
destination = Path(args[args.index("-o") + 1])
profile = Path(args[args.index("-p") + 1]).read_text()
data = bytearray(source.read_bytes())
if "Enabled=true" in profile:
    ifd = struct.unpack_from("<I", data, 4)[0]
    count = struct.unpack_from("<H", data, ifd)[0]
    for index in range(count):
        tag, kind, number, value = struct.unpack_from("<HHII", data, ifd + 2 + index * 12)
        if tag == 273:
            sample = struct.unpack_from("<H", data, value)[0]
            struct.pack_into("<H", data, value, sample ^ 2)
            break
    print("RGB_denoise uses 1 main thread(s) and up to 1 nested thread(s) for each main thread")
    print("RGB_denoise merge buffers: rows=1 cols=2 buffers=2 tile=4x3")
destination.write_bytes(data)
"""


class MetricsTests(unittest.TestCase):
    def test_tiled_layout_is_nonvacuous(self):
        for fixture, marker in (
            ("one-row", (1, 5, 5, 948, 512)),
            ("one-column", (3, 1, 2, 512, 896)),
            ("odd-band", (3, 2, 4, 640, 726)),
        ):
            HARNESS.validate_tile_markers([marker], fixture, "candidate")
            with self.assertRaises(ValueError):
                HARNESS.validate_tile_markers(
                    [(1, 1, 1, 1024, 1024)], fixture, "candidate"
                )
        with self.assertRaises(ValueError):
            HARNESS.validate_tile_markers([], "one-row", "candidate")
        with self.assertRaises(ValueError):
            HARNESS.validate_tile_markers([(1, 2, 0, 4, 3)], "one-row", "candidate")
        HARNESS.validate_tile_markers([], "one-row", "base")
        with self.assertRaises(ValueError):
            HARNESS.validate_tile_markers(
                [(2, 2, 4, 640, 726)], "odd-band", "candidate"
            )

    def test_preflight_errors_preserve_report_without_exports(self):
        for error_type in (subprocess.CalledProcessError, subprocess.TimeoutExpired):
            with tempfile.TemporaryDirectory() as tmp:
                args = SimpleNamespace(
                    base_cli=Path(sys.executable),
                    candidate_cli=Path(sys.executable),
                    scratch=Path(tmp) / "run",
                    timeout=1,
                )
                private_python = "/private-venv/bin/python"
                error = (
                    error_type(1, [private_python, "-c", "import numpy"])
                    if error_type is subprocess.CalledProcessError
                    else error_type([private_python, "-c", "import numpy"], 1)
                )
                with patch.object(
                    HARNESS.sys, "executable", private_python
                ), patch.object(
                    HARNESS.shutil, "which", return_value="magick"
                ), patch.object(
                    HARNESS.subprocess, "run", side_effect=error
                ), patch.object(
                    HARNESS, "write_fixture"
                ) as writer:
                    self.assertEqual(HARNESS.run_harness(args), 1)
                    writer.assert_not_called()
                report = json.loads((args.scratch / "report.json").read_text())
                self.assertEqual(report["status"], "FAIL")
                self.assertEqual(report["exports"], [])
                self.assertIn("$PYTHON", report["error"])
                self.assertNotIn(private_python, report["error"])

    def test_exact_and_one_lsb_success_exit(self):
        self.assertTrue(
            HARNESS.compare_exact(HARNESS.parse_comparison("0 0 0 36", 0, 36))
        )
        metrics = HARNESS.parse_comparison("1 1.5259022e-05 1 36", 0, 36)
        self.assertFalse(HARNESS.compare_exact(metrics))

    def test_invalid_metrics_fail_closed(self):
        for output, code in [
            ("", 0),
            ("0 nan 0 36", 0),
            ("0 inf 0 36", 0),
            ("0 0 0 35", 0),
            ("0 0 1 36", 0),
            ("0 0 0 36", 2),
            ("0 0 0 36", 1),
            ("-1 0 0 36", 0),
            ("0 0 -1 36", 0),
        ]:
            with self.subTest(output=output, code=code), self.assertRaises(ValueError):
                HARNESS.parse_comparison(output, code, 36)

    def test_environment_overrides_conflicting_user_settings(self):
        with patch.dict(
            os.environ,
            {"OMP_NUM_THREADS": "8", "RT_STAGE_OPENCL": "1", "RT_WORK_ORDER": "off"},
        ):
            env = HARNESS.make_environment(Path("settings"), Path("cache"), 1)
        self.assertEqual(env["OMP_NUM_THREADS"], "1")
        self.assertEqual(env["OMP_THREAD_LIMIT"], "1")
        self.assertEqual(env["OMP_MAX_ACTIVE_LEVELS"], "1")
        self.assertEqual(env["RT_STAGE_OPENCL"], "0")
        self.assertEqual(env["RT_TILE_ORDER"], "1")
        self.assertNotIn("RT_WORK_ORDER", env)

    def test_fixture_is_deterministic_rgb16(self):
        with tempfile.TemporaryDirectory() as tmp:
            a, b = Path(tmp) / "a.tif", Path(tmp) / "b.tif"
            HARNESS.write_fixture(a, 4, 3)
            HARNESS.write_fixture(b, 4, 3)
            self.assertEqual(a.read_bytes(), b.read_bytes())
            self.assertEqual(HARNESS.read_tiff_geometry(a), (4, 3))

    def test_scratch_is_not_overwritten(self):
        with tempfile.TemporaryDirectory() as tmp:
            sentinel = Path(tmp) / "keep.txt"
            sentinel.write_text("keep")
            args = SimpleNamespace(scratch=Path(tmp))
            with self.assertRaisesRegex(ValueError, "new or empty"):
                HARNESS.run_harness(args)
            self.assertEqual(sentinel.read_text(), "keep")


@unittest.skipUnless(DEPENDENCIES, "requires NumPy and ImageMagick magick")
class ComparatorIntegrationTests(unittest.TestCase):
    def test_real_one_sample_mutation_fails_harness_gate(self):
        with tempfile.TemporaryDirectory() as tmp:
            scratch = Path(tmp)
            name = "tiny-limit1-order0"
            a = scratch / "base" / name / "output.tif"
            b = scratch / "candidate" / name / "output.tif"
            a.parent.mkdir(parents=True)
            b.parent.mkdir(parents=True)
            HARNESS.write_fixture(a, 4, 3)
            data = bytearray(a.read_bytes())
            # The fixture writer stores its single RGB strip at the end.
            offset = len(data) - 4 * 3 * 6
            value = struct.unpack_from("<H", data, offset)[0]
            struct.pack_into("<H", data, offset, value ^ 1)
            b.write_bytes(data)
            report = {"comparisons": []}
            with patch.dict(HARNESS.FIXTURES, {"tiny": (4, 3)}, clear=True):
                with self.assertRaisesRegex(ValueError, "raster samples differ"):
                    HARNESS.compare_outputs(
                        SimpleNamespace(timeout=30), scratch, COMPARATOR, report
                    )
            self.assertEqual(report["comparisons"][0]["differing"], 1)
            self.assertEqual(report["comparisons"][0]["max_abs"], 1)
            self.assertEqual(report["comparisons"][0]["exit"], 0)

    def test_complete_harness_rejects_one_lsb_candidate(self):
        with tempfile.TemporaryDirectory() as tmp:
            directory = Path(tmp)
            base, candidate = directory / "base cli", directory / "candidate cli"
            base.write_text(FAKE_CLI)
            candidate.write_text(FAKE_CLI.replace("sample ^ 2", "sample ^ 3"))
            base.chmod(0o755)
            candidate.chmod(0o755)
            args = SimpleNamespace(
                base_cli=base,
                candidate_cli=candidate,
                scratch=directory / "run",
                timeout=30,
            )
            with patch.dict(HARNESS.FIXTURES, {"tiny": (4, 3)}, clear=True):
                self.assertEqual(HARNESS.run_harness(args), 1)
            report = json.loads((args.scratch / "report.json").read_text())
            self.assertEqual(report["status"], "FAIL")
            self.assertIn("raster samples differ", report["error"])
            self.assertEqual(report["comparisons"][0]["differing"], 1)
            self.assertEqual(report["comparisons"][0]["max_abs"], 1)
            self.assertEqual(report["comparisons"][0]["exit"], 0)

    def test_same_binary_repeat_mutation_is_determinism_failure(self):
        for role in ("base", "candidate"):
            with self.subTest(role=role), tempfile.TemporaryDirectory() as tmp:
                directory = Path(tmp)
                cli = directory / "fake cli"
                cli.write_text(
                    FAKE_CLI.replace(
                        "sample ^ 2",
                        f"sample ^ (3 if '{role}-repeat' in str(destination) else 2)",
                    )
                )
                cli.chmod(0o755)
                args = SimpleNamespace(
                    base_cli=cli,
                    candidate_cli=cli,
                    scratch=directory / "run",
                    timeout=30,
                )
                with patch.dict(HARNESS.FIXTURES, {"tiny": (4, 3)}, clear=True):
                    self.assertEqual(HARNESS.run_harness(args), 1)
                report = json.loads((args.scratch / "report.json").read_text())
                self.assertIn("determinism", report["error"])
                self.assertEqual(report["comparisons"], [])
                result = report["determinism_comparisons"][-1]
                self.assertEqual(result["role"], role)
                self.assertEqual(result["differing"], 1)
                self.assertEqual(result["max_abs"], 1)
                self.assertEqual(result["exit"], 0)
                self.assertTrue((args.scratch / result["repeat_output"]).is_file())

    def test_complete_harness_with_spaces_and_failed_cli(self):
        with tempfile.TemporaryDirectory(prefix="denoise harness ") as tmp:
            directory = Path(tmp)
            cli = directory / "fake cli"
            cli.write_text(FAKE_CLI)
            cli.chmod(0o755)
            args = SimpleNamespace(
                base_cli=cli,
                candidate_cli=cli,
                scratch=directory / "successful run",
                timeout=30,
            )
            with patch.dict(HARNESS.FIXTURES, {"tiny": (4, 3)}, clear=True):
                self.assertEqual(HARNESS.run_harness(args), 0)
                report = json.loads((args.scratch / "report.json").read_text())
                self.assertEqual(len(report["exports"]), 18)
                self.assertEqual(len(report["determinism_comparisons"]), 8)
                self.assertTrue(
                    all(
                        len(record["output_file_sha256"]) == 64
                        for record in report["exports"]
                    )
                )
                self.assertEqual(len(report["comparisons"]), 4)
                self.assertNotIn(str(directory), json.dumps(report))
                cli.write_text("#!/usr/bin/env python3\nraise SystemExit(7)\n")
                args.scratch = directory / "failed run"
                self.assertEqual(HARNESS.run_harness(args), 1)
                self.assertTrue((args.scratch / "base/tiny-disabled/cli.log").exists())
                report = json.loads((args.scratch / "report.json").read_text())
                self.assertIn("CLI failed", report["error"])


if __name__ == "__main__":
    unittest.main()
