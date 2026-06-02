#!/usr/bin/env python3
"""Run bundled ps1-tests against the local emulator.

The runner has two validation levels:
  * every discovered PS-X EXE is executed headless as a smoke test;
  * tests with a reference VRAM PNG dump the emulator VRAM and compare it with
    tests/tools/diffvram.

It intentionally avoids external Python packages so the Makefile target works
on a fresh checkout.
"""

from __future__ import annotations

import argparse
import os
import struct
import subprocess
import sys
import zlib
from dataclasses import dataclass
from pathlib import Path
from typing import Iterable


ROOT = Path(__file__).resolve().parents[1]
TESTS = ROOT / "tests"
DEFAULT_OUT = TESTS / "out" / "psx-tests"
DIFFVRAM = TESTS / "tools" / "diffvram" / "diffvram-linux-amd64"


@dataclass(frozen=True)
class TestCase:
    category: str
    name: str
    exe: Path
    ref_vram: Path | None = None


def rel(path: Path) -> str:
    return str(path.relative_to(ROOT))


def find_tests() -> list[TestCase]:
    cases: list[TestCase] = []
    skip_dirs = {
        TESTS / "tools",
        TESTS / "out",
    }

    for exe in sorted(TESTS.rglob("*.exe")):
        if any(exe.is_relative_to(skip) for skip in skip_dirs):
            continue
        rel_parts = exe.relative_to(TESTS).parts
        if len(rel_parts) < 2:
            continue

        category = rel_parts[0]
        test_dir = exe.parent
        name_parts = list(exe.relative_to(TESTS / category).with_suffix("").parts)
        if len(name_parts) > 1 and name_parts[-1] == name_parts[-2]:
            name_parts.pop()
        name = "/".join(name_parts)
        ref = reference_for_exe(exe, test_dir)
        cases.append(TestCase(category=category, name=name, exe=exe, ref_vram=ref))

    return cases


def reference_for_exe(exe: Path, test_dir: Path) -> Path | None:
    exact = test_dir / "vram.png"
    if exact.exists():
        return exact

    stem = exe.stem
    if "15bit" in stem:
        ref = test_dir / "vram-15bit.png"
        if ref.exists():
            return ref
    if "24bit" in stem:
        ref = test_dir / "vram-24bit.png"
        if ref.exists():
            return ref

    return None


def filter_cases(cases: Iterable[TestCase], categories: set[str], names: set[str]) -> list[TestCase]:
    out = []
    for case in cases:
        if categories and case.category not in categories:
            continue
        full_name = f"{case.category}/{case.name}"
        if names and case.name not in names and full_name not in names:
            continue
        out.append(case)
    return out


def read_ppm(path: Path) -> tuple[int, int, bytes]:
    data = path.read_bytes()
    pos = 0

    def token() -> bytes:
        nonlocal pos
        while pos < len(data) and data[pos] in b" \t\r\n":
            pos += 1
        if pos < len(data) and data[pos] == ord("#"):
            while pos < len(data) and data[pos] not in b"\r\n":
                pos += 1
            return token()
        start = pos
        while pos < len(data) and data[pos] not in b" \t\r\n":
            pos += 1
        return data[start:pos]

    magic = token()
    if magic != b"P6":
        raise ValueError(f"{path}: unsupported PPM magic {magic!r}")
    width = int(token())
    height = int(token())
    maxval = int(token())
    if maxval != 255:
        raise ValueError(f"{path}: unsupported maxval {maxval}")
    if pos < len(data) and data[pos] in b" \t\r\n":
        pos += 1
    pixels = data[pos:]
    expected = width * height * 3
    if len(pixels) != expected:
        raise ValueError(f"{path}: expected {expected} RGB bytes, got {len(pixels)}")
    return width, height, pixels


def png_chunk(kind: bytes, payload: bytes) -> bytes:
    crc = zlib.crc32(kind)
    crc = zlib.crc32(payload, crc)
    return struct.pack(">I", len(payload)) + kind + payload + struct.pack(">I", crc & 0xFFFFFFFF)


def ppm_to_png(ppm: Path, png: Path) -> None:
    width, height, pixels = read_ppm(ppm)
    rows = []
    stride = width * 3
    grayscale = all(
        pixels[i] == pixels[i + 1] == pixels[i + 2]
        for i in range(0, len(pixels), 3)
    )
    if grayscale:
        for y in range(height):
            row = pixels[y * stride : (y + 1) * stride]
            rows.append(b"\x00" + row[0::3])
        color_type = 0
    else:
        for y in range(height):
            rows.append(b"\x00" + pixels[y * stride : (y + 1) * stride])
        color_type = 2
    raw = b"".join(rows)
    ihdr = struct.pack(">IIBBBBB", width, height, 8, color_type, 0, 0, 0)
    png.write_bytes(
        b"\x89PNG\r\n\x1a\n"
        + png_chunk(b"IHDR", ihdr)
        + png_chunk(b"IDAT", zlib.compress(raw, 9))
        + png_chunk(b"IEND", b"")
    )


def run_case(args: argparse.Namespace, case: TestCase) -> bool:
    case_out = args.out / case.category / case.name.replace("/", "__")
    case_out.mkdir(parents=True, exist_ok=True)
    log_path = case_out / "run.log"
    ppm_path = case_out / "vram.ppm"
    png_path = case_out / "vram.png"
    diff_path = case_out / "diff.png"

    env = os.environ.copy()
    if case.ref_vram and not args.no_diff:
        env["PS1_DUMP_VRAM_PPM"] = str(ppm_path)
        env["PS1_DUMP_FULL_VRAM"] = "1"
        env["PS1_DUMP_FRAME"] = str(args.dump_frame)

    cmd = [
        str(args.emulator),
        "--bios",
        str(args.bios),
        "--exe",
        str(case.exe),
        "--headless",
        "--max-instructions",
        str(args.max_instructions),
    ]

    print(f"[RUN] {case.category}/{case.name}")
    with log_path.open("wb") as log_file:
        proc = subprocess.run(
            cmd,
            cwd=ROOT,
            env=env,
            stdout=log_file,
            stderr=subprocess.STDOUT,
            timeout=args.timeout,
            check=False,
        )

    if proc.returncode != 0:
        print(f"[FAIL] {case.category}/{case.name}: emulator exit {proc.returncode} ({rel(log_path)})")
        return False

    if case.ref_vram and not args.no_diff:
        if not ppm_path.exists():
            print(f"[FAIL] {case.category}/{case.name}: missing VRAM dump ({rel(log_path)})")
            return False
        ppm_to_png(ppm_path, png_path)
        diff_cmd = [str(args.diffvram), str(case.ref_vram), str(png_path), str(diff_path)]
        diff = subprocess.run(diff_cmd, cwd=ROOT, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        if diff.returncode != 0:
            (case_out / "diffvram.log").write_bytes(diff.stdout)
            print(
                f"[FAIL] {case.category}/{case.name}: VRAM differs "
                f"ref={rel(case.ref_vram)} got={rel(png_path)} diff={rel(diff_path)}"
            )
            return False
        print(f"[ OK ] {case.category}/{case.name}: VRAM match")
    else:
        print(f"[ OK ] {case.category}/{case.name}: smoke")

    return True


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--emulator", type=Path, default=ROOT / "ps1_boot")
    parser.add_argument("--bios", type=Path, default=ROOT / "bios" / "BIOS.ROM")
    parser.add_argument("--diffvram", type=Path, default=DIFFVRAM)
    parser.add_argument("--out", type=Path, default=DEFAULT_OUT)
    parser.add_argument("--category", action="append", default=[])
    parser.add_argument("--name", action="append", default=[])
    parser.add_argument("--max-instructions", type=int, default=int(os.getenv("PSX_TEST_MAX_INSTRUCTIONS", "20000000")))
    parser.add_argument("--timeout", type=int, default=int(os.getenv("PSX_TEST_TIMEOUT", "20")))
    parser.add_argument("--dump-frame", type=int, default=int(os.getenv("PSX_TEST_DUMP_FRAME", "120")))
    parser.add_argument("--no-diff", action="store_true")
    parser.add_argument("--keep-going", action="store_true")
    parser.add_argument("--list", action="store_true")
    args = parser.parse_args(argv)

    cases = filter_cases(find_tests(), set(args.category), set(args.name))
    if args.list:
        for case in cases:
            suffix = f" -> {rel(case.ref_vram)}" if case.ref_vram else ""
            print(f"{case.category}/{case.name}{suffix}")
        return 0

    if not cases:
        print("No tests selected", file=sys.stderr)
        return 2

    if not args.emulator.exists():
        print(f"Missing emulator: {args.emulator}", file=sys.stderr)
        return 2
    if not args.bios.exists():
        print(f"Missing BIOS: {args.bios}", file=sys.stderr)
        return 2
    if not args.no_diff and any(case.ref_vram for case in cases) and not args.diffvram.exists():
        print(f"Missing diffvram: {args.diffvram}", file=sys.stderr)
        return 2

    passed = 0
    failed = 0
    for case in cases:
        try:
            ok = run_case(args, case)
        except subprocess.TimeoutExpired:
            print(f"[FAIL] {case.category}/{case.name}: timeout after {args.timeout}s")
            ok = False
        except Exception as exc:
            print(f"[FAIL] {case.category}/{case.name}: {exc}")
            ok = False

        if ok:
            passed += 1
        else:
            failed += 1
            if not args.keep_going:
                break

    print(f"psx-tests: pass={passed} fail={failed} total={passed + failed}")
    return 0 if failed == 0 else 1


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
